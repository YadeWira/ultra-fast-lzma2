/*
 * Copyright (c) 2017-present, Conor McCarthy
 * All rights reserved.
 * Based on zstd.h copyright Yann Collet
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
*/
#if defined (__cplusplus)
extern "C" {
#endif

#ifndef FAST_LZMA2_H
#define FAST_LZMA2_H

/* ======   Dependency   ======*/
#include <stddef.h>   /* size_t */


/* =====   UF2LIB_API : control library symbols visibility   ===== */
#ifndef UF2LIB_VISIBILITY
#  if defined(__GNUC__) && (__GNUC__ >= 4)
#    define UF2LIB_VISIBILITY __attribute__ ((visibility ("default")))
#  else
#    define UF2LIB_VISIBILITY
#  endif
#endif
#if defined(UF2_DLL_EXPORT) && (UF2_DLL_EXPORT==1)
#  define UF2LIB_API __declspec(dllexport) UF2LIB_VISIBILITY
#elif defined(UF2_DLL_IMPORT) && (UF2_DLL_IMPORT==1)
#  define UF2LIB_API __declspec(dllimport) UF2LIB_VISIBILITY /* It isn't required but allows to generate better code, saving a function pointer load from the IAT and an indirect jump.*/
#else
#  define UF2LIB_API UF2LIB_VISIBILITY
#endif

/* ======   Calling convention   ======*/

#if !defined _WIN32 || defined __x86_64__s || defined _M_X64 || (defined __SIZEOF_POINTER__ && __SIZEOF_POINTER__ == 8)
#  define UF2LIB_CALL
#elif defined(__GNUC__)
#  define UF2LIB_CALL __attribute__((cdecl))
#elif defined(_MSC_VER)
#  define UF2LIB_CALL __cdecl
#else
#  define UF2LIB_CALL
#endif

/*******************************************************************************************************
Introduction

*********************************************************************************************************/

/*------   Version   ------*/
#define UF2_VERSION_MAJOR    1
#define UF2_VERSION_MINOR    0
#define UF2_VERSION_RELEASE  1

#define UF2_VERSION_NUMBER  (UF2_VERSION_MAJOR *100*100 + UF2_VERSION_MINOR *100 + UF2_VERSION_RELEASE)
UF2LIB_API unsigned UF2LIB_CALL UF2_versionNumber(void);   /**< useful to check dll version */

#define UF2_LIB_VERSION UF2_VERSION_MAJOR.UF2_VERSION_MINOR.UF2_VERSION_RELEASE
#define UF2_QUOTE(str) #str
#define UF2_EXPAND_AND_QUOTE(str) UF2_QUOTE(str)
#define UF2_VERSION_STRING UF2_EXPAND_AND_QUOTE(UF2_LIB_VERSION)
UF2LIB_API const char* UF2LIB_CALL UF2_versionString(void);


#define UF2_MAXTHREADS 200


/***************************************
*  Simple API
***************************************/

/*! UF2_compress() :
 *  Compresses `src` content as a single LZMA2 compressed stream into already allocated `dst`.
 *  Call UF2_compressMt() to use > 1 thread. Specify nbThreads = 0 to use all cores.
 *  @return : compressed size written into `dst` (<= `dstCapacity),
 *            or an error code if it fails (which can be tested using UF2_isError()). */
UF2LIB_API size_t UF2LIB_CALL UF2_compress(void* dst, size_t dstCapacity,
    const void* src, size_t srcSize,
    int compressionLevel);

UF2LIB_API size_t UF2LIB_CALL UF2_compressMt(void* dst, size_t dstCapacity,
    const void* src, size_t srcSize,
    int compressionLevel,
    unsigned nbThreads);

/*! UF2_decompress() :
 *  Decompresses a single LZMA2 compressed stream from `src` into already allocated `dst`.
 *  `compressedSize` : must be at least the size of the LZMA2 stream.
 *  `dstCapacity` is the original, uncompressed size to regenerate, returned by calling
 *  UF2_findDecompressedSize().
 *  Call UF2_decompressMt() to use > 1 thread. Specify nbThreads = 0 to use all cores. The stream
 *  must contain dictionary resets to use multiple threads. These are inserted during compression by
 *  default. The frequency can be changed/disabled with the UF2_p_resetInterval parameter setting.
 *  @return : the number of bytes decompressed into `dst` (<= `dstCapacity`),
 *            or an errorCode if it fails (which can be tested using UF2_isError()). */
UF2LIB_API size_t UF2LIB_CALL UF2_decompress(void* dst, size_t dstCapacity,
    const void* src, size_t compressedSize);

UF2LIB_API size_t UF2LIB_CALL UF2_decompressMt(void* dst, size_t dstCapacity,
    const void* src, size_t compressedSize,
    unsigned nbThreads);

/*! UF2_findDecompressedSize()
 *  `src` should point to the start of a LZMA2 encoded stream.
 *  `srcSize` must be at least as large as the LZMA2 stream including end marker.
 *  A property byte is assumed to exist at position 0 in `src`. If the stream was created without one,
 *  subtract 1 byte from `src` when passing it to the function.
 *  @return : - decompressed size of the stream in `src`, if known
 *            - UF2_CONTENTSIZE_ERROR if an error occurred (e.g. corruption, srcSize too small)
 *   note 1 : a 0 return value means the stream is valid but "empty".
 *   note 2 : decompressed size can be very large (64-bits value),
 *            potentially larger than what local system can handle as a single memory segment.
 *            In which case, it's necessary to use streaming mode to decompress data.
 *   note 5 : If source is untrusted, decompressed size could be wrong or intentionally modified.
 *            Always ensure return value fits within application's authorized limits.
 *            Each application can set its own limits. */
#define UF2_CONTENTSIZE_ERROR (size_t)-1
UF2LIB_API unsigned long long UF2LIB_CALL UF2_findDecompressedSize(const void *src, size_t srcSize);


/*======  Helper functions  ======*/
UF2LIB_API size_t      UF2LIB_CALL UF2_compressBound(size_t srcSize); /*!< maximum compressed size in worst case scenario */
UF2LIB_API unsigned    UF2LIB_CALL UF2_isError(size_t code);          /*!< tells if a `size_t` function result is an error code */
UF2LIB_API unsigned    UF2LIB_CALL UF2_isTimedOut(size_t code);       /*!< tells if a `size_t` function result is the timeout code */
UF2LIB_API const char* UF2LIB_CALL UF2_getErrorName(size_t code);     /*!< provides readable string from an error code */
UF2LIB_API int         UF2LIB_CALL UF2_maxCLevel(void);               /*!< maximum compression level available */
UF2LIB_API int         UF2LIB_CALL UF2_maxHighCLevel(void);           /*!< maximum compression level available in high mode */


/***************************************
*  Explicit memory management
***************************************/

/*= Compression context
 *  When compressing many times, it is recommended to allocate a context just once,
 *  and re-use it for each successive compression operation. This will make workload
 *  friendlier for system's memory. The context may not use the number of threads requested
 *  if the library is compiled for single-threaded compression or nbThreads > UF2_MAXTHREADS.
 *  Call UF2_getCCtxThreadCount to obtain the actual number allocated. */
typedef struct UF2_CCtx_s UF2_CCtx;
UF2LIB_API UF2_CCtx* UF2LIB_CALL UF2_createCCtx(void);
UF2LIB_API UF2_CCtx* UF2LIB_CALL UF2_createCCtxMt(unsigned nbThreads);
UF2LIB_API void      UF2LIB_CALL UF2_freeCCtx(UF2_CCtx* cctx);

UF2LIB_API unsigned UF2LIB_CALL UF2_getCCtxThreadCount(const UF2_CCtx* cctx);

/*! UF2_compressCCtx() :
 *  Same as UF2_compress(), but requires an allocated UF2_CCtx (see UF2_createCCtx()). */
UF2LIB_API size_t UF2LIB_CALL UF2_compressCCtx(UF2_CCtx* cctx,
    void* dst, size_t dstCapacity,
    const void* src, size_t srcSize,
    int compressionLevel);

/*! UF2_getCCtxDictProp() :
 *  Get the dictionary size property.
 *  Intended for use with the UF2_p_omitProperties parameter for creating a
 *  7-zip or XZ compatible LZMA2 stream. */
UF2LIB_API unsigned char UF2LIB_CALL UF2_getCCtxDictProp(UF2_CCtx* cctx);


/****************************
*  Decompression
****************************/

/*= Decompression context
 *  When decompressing many times, it is recommended to allocate a context only once,
 *  and re-use it for each successive decompression operation. This will make the workload
 *  friendlier for the system's memory.
 *  The context may not allocate the number of threads requested if the library is
 *  compiled for single-threaded compression or nbThreads > UF2_MAXTHREADS.
 *  Call UF2_getDCtxThreadCount to obtain the actual number allocated.
 *  At least nbThreads dictionary resets must exist in the stream to use all of the
 *  threads. Dictionary resets are inserted into the stream according to the
 *  UF2_p_resetInterval parameter used in the compression context. */
typedef struct UF2_DCtx_s UF2_DCtx;
UF2LIB_API UF2_DCtx* UF2LIB_CALL UF2_createDCtx(void);
UF2LIB_API UF2_DCtx* UF2LIB_CALL UF2_createDCtxMt(unsigned nbThreads);
UF2LIB_API size_t    UF2LIB_CALL UF2_freeDCtx(UF2_DCtx* dctx);

UF2LIB_API unsigned UF2LIB_CALL UF2_getDCtxThreadCount(const UF2_DCtx* dctx);


/*! UF2_initDCtx() :
 *  Use only when a property byte is not present at input byte 0. No init is necessary otherwise.
 *  The caller must store the result from UF2_getCCtxDictProp() and pass it to this function. */
UF2LIB_API size_t UF2LIB_CALL UF2_initDCtx(UF2_DCtx* dctx, unsigned char prop);

/*! UF2_decompressDCtx() :
 *  Same as UF2_decompress(), requires an allocated UF2_DCtx (see UF2_createDCtx()) */
UF2LIB_API size_t UF2LIB_CALL UF2_decompressDCtx(UF2_DCtx* cctx,
    void* dst, size_t dstCapacity,
    const void* src, size_t srcSize);

/****************************
*  Streaming
****************************/

typedef struct {
    const void* src;    /**< start of input buffer */
    size_t size;        /**< size of input buffer */
    size_t pos;         /**< position where reading stopped. Will be updated. Necessarily 0 <= pos <= size */
} UF2_inBuffer;

typedef struct {
    void*  dst;         /**< start of output buffer */
    size_t size;        /**< size of output buffer */
    size_t pos;         /**< position where writing stopped. Will be updated. Necessarily 0 <= pos <= size */
} UF2_outBuffer;

/*** Push/pull structs ***/

typedef struct {
    void*  dst;         /**< start of available dict buffer */
    unsigned long size; /**< size of dict remaining */
} UF2_dictBuffer;

typedef struct {
    const void* src;    /**< start of compressed data */
    size_t size;        /**< size of compressed data */
} UF2_cBuffer;

/*-***********************************************************************
 *  Streaming compression
 *
 *  A UF2_CStream object is required to track streaming operation.
 *  Use UF2_createCStream() and UF2_freeCStream() to create/release resources.
 *  UF2_CStream objects can be reused multiple times on consecutive compression operations.
 *  It is recommended to re-use UF2_CStream in situations where many streaming operations will be done
 *  consecutively, since it will reduce allocation and initialization time.
 *
 *  Call UF2_createCStreamMt() with a nonzero dualBuffer parameter to use two input dictionary buffers.
 *  The stream will not block on UF2_compressStream() and continues to accept data while compression is
 *  underway, until both buffers are full. Useful when I/O is slow.
 *  To compress with a single thread with dual buffering, call UF2_createCStreamMt with nbThreads=1.
 *
 *  Use UF2_initCStream() on the UF2_CStream object to start a new compression operation.
 *
 *  Use UF2_compressStream() repetitively to consume input stream.
 *  The function will automatically update the `pos` field.
 *  It will always consume the entire input unless an error occurs or the dictionary buffer is filled,
 *  unlike the decompression function.
 *
 *  The radix match finder allows compressed data to be stored in its match table during encoding.
 *  Applications may call streaming compression functions with output == NULL. In this case,
 *  when the function returns 1, the compressed data must be read from the internal buffers.
 *  Call UF2_getNextCompressedBuffer() repeatedly until it returns 0.
 *  Each call returns buffer information in the UF2_inBuffer parameter. Applications typically will 
 *  passed this to an I/O write function or downstream filter.
 *  Alternately, applications may pass an UF2_outBuffer object pointer to receive the output. In this
 *  case the return value is 1 if the buffer is full and more compressed data remains.
 *
 *  UF2_endStream() instructs to finish a stream. It will perform a flush and write the LZMA2
 *  termination byte (required). Call UF2_endStream() repeatedly until it returns 0.
 *
 *  Most functions may return a size_t error code, which can be tested using UF2_isError().
 *
 * *******************************************************************/

typedef struct UF2_CCtx_s UF2_CStream;

/*===== UF2_CStream management functions =====*/
UF2LIB_API UF2_CStream* UF2LIB_CALL UF2_createCStream(void);
UF2LIB_API UF2_CStream* UF2LIB_CALL UF2_createCStreamMt(unsigned nbThreads, int dualBuffer);
UF2LIB_API void UF2LIB_CALL UF2_freeCStream(UF2_CStream * fcs);

/*===== Streaming compression functions =====*/

/*! UF2_initCStream() :
 *  Call this function before beginning a new compressed data stream. To keep the stream object's
 *  current parameters, specify zero for the compression level. The object is set to the default
 *  level upon creation. */
UF2LIB_API size_t UF2LIB_CALL UF2_initCStream(UF2_CStream* fcs, int compressionLevel);

/*! UF2_setCStreamTimeout() :
 *  Sets a timeout in milliseconds. Zero disables the timeout (default). If a nonzero timout is set, functions
 *  UF2_compressStream(), UF2_getDictionaryBuffer(), UF2_updateDictionary(), UF2_getNextCompressedBuffer(),
 *  UF2_flushStream(), and UF2_endStream() may return a timeout code before compression of the current
 *  dictionary of data completes. UF2_isError() returns true for the timeout code, so check the code with
 *  UF2_isTimedOut() before testing for errors. With the exception of UF2_updateDictionary(), the above
 *  functions may be called again to wait for completion. A typical application for timeouts is to update the
 *  user on compression progress. */
UF2LIB_API size_t UF2LIB_CALL UF2_setCStreamTimeout(UF2_CStream * fcs, unsigned timeout);

/*! UF2_compressStream() :
 *  Reads data from input into the dictionary buffer. Compression will begin if the buffer fills up.
 *  A dual buffering stream will fill the second buffer while compression proceeds on the first.
 *  A call to UF2_compressStream() will wait for ongoing compression to complete if all dictionary space
 *  is filled. UF2_compressStream() must not be called with output == NULL unless the caller has read all
 *  compressed data from the CStream object.
 *  Returns 1 to indicate compressed data must be read (or output is full), or 0 otherwise. */
UF2LIB_API size_t UF2LIB_CALL UF2_compressStream(UF2_CStream* fcs, UF2_outBuffer *output, UF2_inBuffer* input);

/*! UF2_copyCStreamOutput() :
 *  Copies compressed data to the output buffer until the buffer is full or all available data is copied.
 *  If asynchronous compression is in progress, the function returns 0 without waiting.
 *  Returns 1 to indicate some compressed data remains, or 0 otherwise. */
UF2LIB_API size_t UF2LIB_CALL UF2_copyCStreamOutput(UF2_CStream* fcs, UF2_outBuffer *output);

/*** Push/pull functions ***/

/*! UF2_getDictionaryBuffer() :
 *  Returns a buffer in the UF2_outBuffer object, which the caller can directly read data into.
 *  Applications will normally pass this buffer to an I/O read function or upstream filter.
 *  Returns 0, or an error or timeout code. */
UF2LIB_API size_t UF2LIB_CALL UF2_getDictionaryBuffer(UF2_CStream* fcs, UF2_dictBuffer* dict);

/*! UF2_updateDictionary() :
 *  Informs the CStream how much data was added to the buffer. Compression begins if the dictionary
 *  was filled. Returns 1 to indicate compressed data must be read, 0 if not, or an error code. */
UF2LIB_API size_t UF2LIB_CALL UF2_updateDictionary(UF2_CStream* fcs, size_t addedSize);

/*! UF2_getNextCompressedBuffer() :
 *  Returns a buffer containing a slice of the compressed data. Call this function and process the data
 *  until the function returns zero. In most cases it will return a buffer for each compression thread
 *  used. It is sometimes less but never more than nbThreads. If asynchronous compression is in progress,
 *  this function will wait for completion before returning, or it will return the timeout code. */
UF2LIB_API size_t UF2LIB_CALL UF2_getNextCompressedBuffer(UF2_CStream* fcs, UF2_cBuffer* cbuf);

/******/

/*! UF2_getCStreamProgress() :
 *  Returns the number of bytes processed since the stream was initialized. This is a synthetic
 *  estimate because the match finder does not proceed sequentially through the data. If
 *  outputSize is not NULL, returns the number of bytes of compressed data generated. */
UF2LIB_API unsigned long long UF2LIB_CALL UF2_getCStreamProgress(const UF2_CStream * fcs, unsigned long long *outputSize);

/*! UF2_waitCStream() :
 *  Waits for compression to end. This function returns after the timeout set using
 *  UF2_setCStreamTimeout has elapsed. Unnecessary when no timeout is set.
 *  Returns 1 if compressed output is available, 0 if not, or the timeout code. */
UF2LIB_API size_t UF2LIB_CALL UF2_waitCStream(UF2_CStream * fcs);

/*! UF2_cancelCStream() :
 *  Cancels any compression operation underway. Useful only when dual buffering and/or timeouts
 *  are enabled. The stream will be returned to an uninitialized state. */
UF2LIB_API void UF2LIB_CALL UF2_cancelCStream(UF2_CStream *fcs);

/*! UF2_remainingOutputSize() :
 *  The amount of compressed data remaining to be read from the CStream object. */
UF2LIB_API size_t UF2LIB_CALL UF2_remainingOutputSize(const UF2_CStream* fcs);

/*! UF2_flushStream() :
 *  Compress all data remaining in the dictionary buffer(s). It may be necessary to call
 *  UF2_flushStream() more than once. If output == NULL the compressed data must be read from the
 *  CStream object after each call.
 *  Flushing is not normally useful and produces larger output.
 *  Returns 1 if input or output still exists in the CStream object, 0 if complete, or an error code. */
UF2LIB_API size_t UF2LIB_CALL UF2_flushStream(UF2_CStream* fcs, UF2_outBuffer *output);

/*! UF2_endStream() :
 *  Compress all data remaining in the dictionary buffer(s) and write the stream end marker. It may
 *  be necessary to call UF2_endStream() more than once. If output == NULL the compressed data must
 *  be read from the CStream object after each call.
 *  Returns 0 when compression is complete and all output has been flushed, 1 if not complete, or
 *  an error code. */
UF2LIB_API size_t UF2LIB_CALL UF2_endStream(UF2_CStream* fcs, UF2_outBuffer *output);

/*-***************************************************************************
 *  Streaming decompression
 *
 *  A UF2_DStream object is required to track streaming operations.
 *  Use UF2_createDStream() and UF2_freeDStream() to create/release resources.
 *  UF2_DStream objects can be re-used multiple times.
 *
 *  Use UF2_initDStream() to start a new decompression operation.
 *  @return : zero or an error code
 *
 *  Use UF2_decompressStream() repetitively to consume your input.
 *  The function will update both `pos` fields.
 *  If `input.pos < input.size`, some input has not been consumed.
 *  It's up to the caller to present again the remaining data.
 *  If `output.pos < output.size`, decoder has flushed everything it could.
 *  @return : 0 when a stream is completely decoded and fully flushed,
 *            1, which means there is still some decoding to do to complete the stream,
 *            or an error code, which can be tested using UF2_isError().
 * *******************************************************************************/

typedef struct UF2_DStream_s UF2_DStream;

/*===== UF2_DStream management functions =====*/
UF2LIB_API UF2_DStream* UF2LIB_CALL UF2_createDStream(void);
UF2LIB_API UF2_DStream* UF2LIB_CALL UF2_createDStreamMt(unsigned nbThreads);
UF2LIB_API size_t UF2LIB_CALL UF2_freeDStream(UF2_DStream* fds);

/*! UF2_setDStreamMemoryLimitMt() :
 *  Set a total size limit for multithreaded decoder input and output buffers. MT decoder memory
 *  usage is unknown until the input is parsed. If the limit is exceeded, the decoder switches to
 *  using a single thread.
 *  MT decoding memory usage is typically dictionary_size * 4 * nbThreads for the output
 *  buffers plus the size of the compressed input for that amount of output. */
UF2LIB_API void UF2LIB_CALL UF2_setDStreamMemoryLimitMt(UF2_DStream* fds, size_t limit);

/*! UF2_setDStreamTimeout() :
 *  Sets a timeout in milliseconds. Zero disables the timeout. If a nonzero timout is set,
 *  UF2_decompressStream() may return a timeout code before decompression of the available data
 *  completes. UF2_isError() returns true for the timeout code, so check the code with UF2_isTimedOut()
 *  before testing for errors. After a timeout occurs, do not call UF2_decompressStream() again unless
 *  a call to UF2_waitDStream() returns 1. A typical application for timeouts is to update the user on
 *  decompression progress. */
UF2LIB_API size_t UF2LIB_CALL UF2_setDStreamTimeout(UF2_DStream * fds, unsigned timeout);

/*! UF2_waitDStream() :
 *  Waits for decompression to end after a timeout has occurred. This function returns after the
 *  timeout set using UF2_setDStreamTimeout() has elapsed, or when decompression of available input is
 *  complete. Unnecessary when no timeout is set.
 *  Returns 0 if the stream is complete, 1 if not complete, or an error code. */
UF2LIB_API size_t UF2LIB_CALL UF2_waitDStream(UF2_DStream * fds);

/*! UF2_cancelDStream() :
 *  Frees memory allocated for MT decoding. If a timeout is set and the caller is waiting
 *  for completion of MT decoding, decompression in progress will be canceled. */
UF2LIB_API void UF2LIB_CALL UF2_cancelDStream(UF2_DStream *fds);

/*! UF2_getDStreamProgress() :
 *  Returns the number of bytes decoded since the stream was initialized. */
UF2LIB_API unsigned long long UF2LIB_CALL UF2_getDStreamProgress(const UF2_DStream * fds);

/*===== Streaming decompression functions =====*/

/*! UF2_initDStream() :
 *  Call this function before decompressing a stream. UF2_initDStream_withProp()
 *  must be used for streams which do not include a property byte at position zero.
 *  The caller is responsible for storing and passing the property byte.
 *  Returns 0 if okay, or an error if the stream object is still in use from a
 *  previous call to UF2_decompressStream() (see timeout info above). */
UF2LIB_API size_t UF2LIB_CALL UF2_initDStream(UF2_DStream* fds);
UF2LIB_API size_t UF2LIB_CALL UF2_initDStream_withProp(UF2_DStream* fds, unsigned char prop);

/*! UF2_decompressStream() :
 *  Reads data from input and decompresses to output.
 *  Returns 1 if the stream is unfinished, 0 if the terminator was encountered (he'll be back)
 *  and all data was written to output, or an error code. Call this function repeatedly if
 *  necessary, removing data from output and/or loading data into input before each call. */
UF2LIB_API size_t UF2LIB_CALL UF2_decompressStream(UF2_DStream* fds, UF2_outBuffer* output, UF2_inBuffer* input);

/*-***************************************************************************
 *  Compression parameters
 *
 *  Any function that takes a 'compressionLevel' parameter will replace any
 *  parameters affected by compression level that are already set.
 *  To use a preset level and modify it, call UF2_CCtx_setParameter with
 *  UF2_p_compressionLevel to set the level, then call UF2_CCtx_setParameter again
 *  with any other settings to change.
 *  Specify a compressionLevel of 0 when calling a compression function to keep
 *  the current parameters.
 * *******************************************************************************/

#define UF2_DICTLOG_MIN      20
#define UF2_DICTLOG_MAX_32   27
#define UF2_DICTLOG_MAX_64   30
#define UF2_DICTLOG_MAX      ((unsigned)(sizeof(size_t) == 4 ? UF2_DICTLOG_MAX_32 : UF2_DICTLOG_MAX_64))
#define UF2_DICTSIZE_MAX     (1U << UF2_DICTLOG_MAX)
#define UF2_DICTSIZE_MIN     (1U << UF2_DICTLOG_MIN)
#define UF2_BLOCK_OVERLAP_MIN 0
#define UF2_BLOCK_OVERLAP_MAX 14
#define UF2_RESET_INTERVAL_MIN 1
#define UF2_RESET_INTERVAL_MAX 16  /* small enough to fit UF2_DICTSIZE_MAX * UF2_RESET_INTERVAL_MAX in 32-bit size_t */
#define UF2_BUFFER_RESIZE_MIN 0
#define UF2_BUFFER_RESIZE_MAX 4
#define UF2_BUFFER_RESIZE_DEFAULT 2
#define UF2_CHAINLOG_MIN       4
#define UF2_CHAINLOG_MAX       14
#define UF2_HYBRIDCYCLES_MIN    1
#define UF2_HYBRIDCYCLES_MAX   64
#define UF2_SEARCH_DEPTH_MIN 6
#define UF2_SEARCH_DEPTH_MAX 254
#define UF2_FASTLENGTH_MIN    6   /* only used by optimizer */
#define UF2_FASTLENGTH_MAX  273   /* only used by optimizer */
#define UF2_LC_MIN 0
#define UF2_LC_MAX 4
#define UF2_LP_MIN 0
#define UF2_LP_MAX 4
#define UF2_PB_MIN 0
#define UF2_PB_MAX 4
#define UF2_LCLP_MAX 4

typedef enum {
    UF2_fast,
    UF2_opt,
    UF2_ultra
} UF2_strategy;

typedef struct {
    size_t   dictionarySize;   /* largest match distance : larger == more compression, more memory needed during decompression; > 64Mb == more memory per byte, slower */
    unsigned overlapFraction;  /* overlap between consecutive blocks in 1/16 units: larger == more compression, slower */
    unsigned chainLog;         /* HC3 sliding window : larger == more compression, slower; hybrid mode only (ultra) */
    unsigned cyclesLog;        /* nb of searches : larger == more compression, slower; hybrid mode only (ultra) */
    unsigned searchDepth;      /* maximum depth for resolving string matches : larger == more compression, slower */
    unsigned fastLength;       /* acceptable match size for parser : larger == more compression, slower; fast bytes parameter from 7-Zip */
    unsigned divideAndConquer; /* split long chains of 2-byte matches into shorter chains with a small overlap : faster, somewhat less compression; enabled by default */
    UF2_strategy strategy;     /* encoder strategy : fast, optimized or ultra (hybrid) */
} UF2_compressionParameters;

typedef enum {
    /* compression parameters */
    UF2_p_compressionLevel, /* Update all compression parameters according to pre-defined cLevel table
                             * Default level is UF2_CLEVEL_DEFAULT==6.
                             * Setting UF2_p_highCompression to 1 switches to an alternate cLevel table. */
    UF2_p_highCompression,  /* Maximize compression ratio for a given dictionary size.
                             * Levels 1..10 = dictionaryLog 20..29 (1 Mb..512 Mb).
                             * Typically provides a poor speed/ratio tradeoff. */
    UF2_p_dictionaryLog,    /* Maximum allowed back-reference distance, expressed as power of 2.
                             * Must be clamped between UF2_DICTLOG_MIN and UF2_DICTLOG_MAX.
                             * Default = 24 */
    UF2_p_dictionarySize,   /* Same as above but expressed as an absolute value. 
                             * Must be clamped between UF2_DICTSIZE_MIN and UF2_DICTSIZE_MAX.
                             * Default = 16 Mb */
    UF2_p_overlapFraction,  /* The radix match finder is block-based, so some overlap is retained from
                             * each block to improve compression of the next. This value is expressed
                             * as n / 16 of the block size (dictionary size). Larger values are slower.
                             * Values above 2 mostly yield only a small improvement in compression.
                             * A large value for a small dictionary may worsen multithreaded compression.
                             * Default = 2 */
    UF2_p_resetInterval,    /* For multithreaded decompression. A dictionary reset will occur
                             * after each dictionarySize * resetInterval bytes of input.
                             * Default = 4 */
    UF2_p_bufferResize,     /* Buffering speeds up the matchfinder. Buffer resize determines the percentage of
                             * the normal buffer size used, which depends on dictionary size.
                             * 0=50, 1=75, 2=100, 3=150, 4=200. Higher number = slower, better
                             * compression, higher memory usage. A CPU with a large memory cache
                             * may make effective use of a larger buffer.
                             * Default = 2 */
    UF2_p_hybridChainLog,   /* Size of the hybrid mode HC3 hash chain, as a power of 2.
                             * Resulting table size is (1 << (chainLog+2)) bytes.
                             * Larger tables result in better and slower compression.
                             * This parameter is only used by the hybrid "ultra" strategy.
                             * Default = 9 */
    UF2_p_hybridCycles,     /* Number of search attempts made by the HC3 match finder.
                             * Used only by the hybrid "ultra" strategy.
                             * More attempts result in slightly better and slower compression.
                             * Default = 1 */
    UF2_p_searchDepth,      /* Match finder will resolve string matches up to this length. If a longer
                             * match exists further back in the input, it will not be found.
                             * Default = 42 */
    UF2_p_fastLength,       /* Only useful for strategies >= opt.
                             * Length of match considered "good enough" to stop search.
                             * Larger values make compression stronger and slower.
                             * Default = 48 */
    UF2_p_divideAndConquer, /* Split long chains of 2-byte matches into shorter chains with a small overlap
                             * for further processing. Allows buffering of all chains at length 2.
                             * Faster, less compression. Generally a good tradeoff.
                             * Default = enabled */
    UF2_p_strategy,         /* 1 = fast; 2 = optimized, 3 = ultra (hybrid mode).
                             * The higher the value of the selected strategy, the more complex it is,
                             * resulting in stronger and slower compression.
                             * Default = ultra */
    UF2_p_literalCtxBits,   /* lc value for LZMA2 encoder
                             * Default = 3 */
    UF2_p_literalPosBits,   /* lp value for LZMA2 encoder
                             * Default = 0 */
    UF2_p_posBits,          /* pb value for LZMA2 encoder
                             * Default = 2 */
    UF2_p_omitProperties,   /* Omit the property byte at the start of the stream. For use within 7-zip */
                            /* or other containers which store the property byte elsewhere. */
                            /* A stream compressed under this setting cannot be decoded by this library. */
#ifndef NO_XXHASH
    UF2_p_doXXHash,         /* Calculate a 32-bit xxhash value from the input data and store it 
                             * after the stream terminator. The value will be checked on decompression.
                             * 0 = do not calculate; 1 = calculate (default) */
#endif
#ifdef RMF_REFERENCE
    UF2_p_useReferenceMF    /* Use the reference matchfinder for development purposes. SLOW. */
#endif
} UF2_cParameter;


/*! UF2_CCtx_setParameter() :
 *  Set one compression parameter, selected by enum UF2_cParameter.
 *  @result : informational value (typically, the one being set, possibly corrected),
 *            or an error code (which can be tested with UF2_isError()). */
UF2LIB_API size_t UF2LIB_CALL UF2_CCtx_setParameter(UF2_CCtx* cctx, UF2_cParameter param, size_t value);

/*! UF2_CCtx_getParameter() :
 *  Get one compression parameter, selected by enum UF2_cParameter.
 *  @result : the parameter value, or the parameter_unsupported error code
 *            (which can be tested with UF2_isError()). */
UF2LIB_API size_t UF2LIB_CALL UF2_CCtx_getParameter(UF2_CCtx* cctx, UF2_cParameter param);

/*! UF2_CStream_setParameter() :
 *  Set one compression parameter, selected by enum UF2_cParameter.
 *  @result : informational value (typically, the one being set, possibly corrected),
 *            or an error code (which can be tested with UF2_isError()). */
UF2LIB_API size_t UF2LIB_CALL UF2_CStream_setParameter(UF2_CStream* fcs, UF2_cParameter param, size_t value);

/*! UF2_CStream_getParameter() :
 *  Get one compression parameter, selected by enum UF2_cParameter.
 *  @result : the parameter value, or the parameter_unsupported error code
 *            (which can be tested with UF2_isError()). */
UF2LIB_API size_t UF2LIB_CALL UF2_CStream_getParameter(UF2_CStream* fcs, UF2_cParameter param);

/*! UF2_getLevelParameters() :
 *  Get all compression parameter values defined by the preset compressionLevel.
 *  @result : the values in a UF2_compressionParameters struct, or the parameter_outOfBound error code
 *            (which can be tested with UF2_isError()) if compressionLevel is invalid. */
UF2LIB_API size_t UF2LIB_CALL UF2_getLevelParameters(int compressionLevel, int high, UF2_compressionParameters *params);


/***************************************
*  Context memory usage
***************************************/

/*! UF2_estimate*() :
*  These functions estimate memory usage of a CCtx before its creation or before any operation has begun.
*  UF2_estimateCCtxSize() will provide a budget large enough for any compression level up to selected one.
*  To use UF2_estimateCCtxSize_usingCCtx, set the compression level and any other settings for the context,
*  then call the function. Some allocation occurs when the context is created, but the large memory buffers
*  used for string matching are allocated only when compression is initialized. */

UF2LIB_API size_t UF2LIB_CALL UF2_estimateCCtxSize(int compressionLevel, unsigned nbThreads); /*!< memory usage determined by level */
UF2LIB_API size_t UF2LIB_CALL UF2_estimateCCtxSize_byParams(const UF2_compressionParameters *params, unsigned nbThreads); /*!< memory usage determined by params */
UF2LIB_API size_t UF2LIB_CALL UF2_estimateCCtxSize_usingCCtx(const UF2_CCtx* cctx);           /*!< memory usage determined by settings */
UF2LIB_API size_t UF2LIB_CALL UF2_estimateCStreamSize(int compressionLevel, unsigned nbThreads, int dualBuffer); /*!< memory usage determined by level */
UF2LIB_API size_t UF2LIB_CALL UF2_estimateCStreamSize_byParams(const UF2_compressionParameters *params, unsigned nbThreads, int dualBuffer); /*!< memory usage determined by params */
UF2LIB_API size_t UF2LIB_CALL UF2_estimateCStreamSize_usingCStream(const UF2_CStream* fcs);   /*!< memory usage determined by settings */

/*! UF2_getDictSizeFromProp() :
 *  Get the dictionary size from the property byte for a stream. The property byte is the first byte
*   in the stream, unless omitProperties was enabled, in which case the caller must store it. */
UF2LIB_API size_t UF2LIB_CALL UF2_getDictSizeFromProp(unsigned char prop);

/*! UF2_estimateDCtxSize() :
 *  The size of a DCtx does not include a dictionary buffer because the caller must supply one. */
UF2LIB_API size_t UF2LIB_CALL UF2_estimateDCtxSize(unsigned nbThreads);

/*! UF2_estimateDStreamSize() :
 *  Estimate decompression memory use from the dictionary size and number of threads.
 *  For nbThreads == 0 the number of available cores will be used.
 *  Obtain dictSize by passing the property byte to UF2_getDictSizeFromProp. */
UF2LIB_API size_t UF2LIB_CALL UF2_estimateDStreamSize(size_t dictSize, unsigned nbThreads); /*!<  obtain dictSize from UF2_getDictSizeFromProp() */

#endif  /* FAST_LZMA2_H */

#if defined (__cplusplus)
}
#endif

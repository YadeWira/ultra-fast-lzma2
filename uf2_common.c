/*
 * Copyright (c) 2016-present, Yann Collet, Facebook, Inc.
 * All rights reserved.
 * Modified for FL2 by Conor McCarthy
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */



/*-*************************************
*  Dependencies
***************************************/
#include <stdlib.h>
#include "uf-lzma2.h"
#include "uf2_xz.h"
#include "uf2_errors.h"
#include "uf2_internal.h"
#include "lzma2_enc.h"


/*-****************************************
*  Version
******************************************/
UF2LIB_API unsigned UF2LIB_CALL UF2_versionNumber(void) { return UF2_VERSION_NUMBER; }

UF2LIB_API const char* UF2LIB_CALL UF2_versionString(void) { return UF2_VERSION_STRING; }


/*-****************************************
*  Custom allocator handlers
******************************************/
void* (*UF2_g_alloc)(size_t size);
void  (*UF2_g_free)(void* address);
void* (*UF2_g_large_alloc)(size_t size);
void  (*UF2_g_large_free)(void* address);
unsigned char UF2_g_alloc_called;

void *UF2_malloc(size_t size)
{
    UF2_g_alloc_called = 1;
    char *address = (UF2_g_alloc != NULL) ? UF2_g_alloc(size) : malloc(size);
    DEBUGLOG(3, "UF2_malloc: %lu bytes at 0x%lX", (long)size, (long)(address - (char*)0));
    return address;
}

void *UF2_calloc(size_t count, size_t size)
{
    size *= count;
    void *block = UF2_malloc(size);
    if (block != NULL)
        memset(block, 0, size);
    return block;
}

void UF2_free(void *address)
{
    DEBUGLOG(3, "UF2_free: 0x%lX", (long)((char*)address - (char*)0));
    if (UF2_g_free != NULL)
        UF2_g_free(address);
    else
        free(address);
}

void *UF2_large_malloc(size_t size)
{
    UF2_g_alloc_called = 1;
    char *address = (UF2_g_large_alloc != NULL) ? UF2_g_large_alloc(size) : UF2_malloc(size);
    DEBUGLOG(3, "UF2_large_malloc: %lu bytes at 0x%lX", (long)size, (long)(address - (char*)0));
    return address;
}

void UF2_large_free(void *address)
{
    DEBUGLOG(3, "UF2_large_free: 0x%lX", (long)((char*)address - (char*)0));
    if (UF2_g_large_free != NULL)
        UF2_g_large_free(address);
    else
        UF2_free(address);
}

UF2LIB_API size_t UF2LIB_CALL UF2_setAllocator(void* (*allocFunction)(size_t size),
    void(*freeFunction)(void* address))
{
    if (UF2_g_alloc_called)
        return UF2_ERROR(stage_wrong);
    UF2_g_alloc = allocFunction;
    UF2_g_free = freeFunction;
    return UF2_error_no_error;
}

UF2LIB_API size_t UF2LIB_CALL UF2_setLargeAllocator(void* (*allocFunction)(size_t size),
    void(*freeFunction)(void* address))
{
    if (UF2_g_alloc_called)
        return UF2_ERROR(stage_wrong);
    UF2_g_large_alloc = allocFunction;
    UF2_g_large_free = freeFunction;
    return UF2_error_no_error;
}


/*-****************************************
*  Compression helpers
******************************************/
UF2LIB_API size_t UF2LIB_CALL UF2_compressBound(size_t srcSize)
{
	/* Large enough for either format. An .xz file's blocks are never smaller than
	 * UF2_XZ_BLOCKSIZE_MIN except for the last, which bounds how many there are. */
	return LZMA2_compressBound(srcSize) + XZ_OVERHEAD_MAX(srcSize / UF2_XZ_BLOCKSIZE_MIN + 1);
}

/*-****************************************
*  UF2 Error Management
******************************************/
HINT_INLINE
unsigned IsError(size_t code)
{
    return (code > UF2_ERROR(maxCode));
}

/*! UF2_isError() :
 *  tells if a return value is an error code */
UF2LIB_API unsigned UF2LIB_CALL UF2_isError(size_t code)
{
    return IsError(code);
}

/*! UF2_isTimedOut() :
 *  tells if a return value is the timeout code */
UF2LIB_API unsigned UF2LIB_CALL UF2_isTimedOut(size_t code)
{
    return (code == UF2_ERROR(timedOut));
}

/*! UF2_getErrorName() :
 *  provides error code string from function result (useful for debugging) */
UF2LIB_API const char* UF2LIB_CALL UF2_getErrorName(size_t code)
{
    return UF2_getErrorString(UF2_getErrorCode(code));
}

/*! UF2_getError() :
 *  convert a `size_t` function result into a proper UF2_errorCode enum */
UF2LIB_API UF2_ErrorCode UF2LIB_CALL UF2_getErrorCode(size_t code)
{
    if (!IsError(code)) 
        return (UF2_ErrorCode)0;

    return (UF2_ErrorCode)(0 - code);
}

/*! UF2_getErrorString() :
 *  provides error code string from enum */
UF2LIB_API const char* UF2LIB_CALL UF2_getErrorString(UF2_ErrorCode code)
{
    static const char* const notErrorCode = "Unspecified error code";
    switch (code)
    {
    case PREFIX(no_error): return "No error detected";
    case PREFIX(GENERIC):  return "Error (generic)";
    case PREFIX(internal): return "Internal error (bug)";
    case PREFIX(corruption_detected): return "Corrupted block detected";
    case PREFIX(checksum_wrong): return "Restored data doesn't match checksum";
    case PREFIX(parameter_unsupported): return "Unsupported parameter";
    case PREFIX(parameter_outOfBound): return "Parameter is out of bound";
    case PREFIX(lclpMax_exceeded): return "Parameters lc+lp > 4";
    case PREFIX(stage_wrong): return "Not possible at this stage of encoding";
    case PREFIX(init_missing): return "Context should be init first";
    case PREFIX(memory_allocation): return "Allocation error : not enough memory";
    case PREFIX(dstSize_tooSmall): return "Destination buffer is too small";
    case PREFIX(srcSize_wrong): return "Src size is incorrect";
    case PREFIX(canceled): return "Processing was canceled by a call to UF2_cancelCStream() or UF2_cancelDStream()";
    case PREFIX(buffer): return "Streaming progress halted due to buffer(s) full/empty";
    case PREFIX(timedOut): return "Wait timed out. Timeouts should be handled before errors using UF2_isTimedOut()";
        /* following error codes are not stable and may be removed or changed in a future version */
    case PREFIX(maxCode):
    default: return notErrorCode;
    }
}

/*! g_debuglog_enable :
 *  turn on/off debug traces (global switch) */
#if defined(UF2_DEBUG) && (UF2_DEBUG >= 2)
int g_debuglog_enable = 1;
#endif


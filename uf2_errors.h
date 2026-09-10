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

#ifndef UF2_ERRORS_H_398273423
#define UF2_ERRORS_H_398273423

#if defined (__cplusplus)
extern "C" {
#endif

/*===== dependency =====*/
#include <stddef.h>   /* size_t */

#include "uf-lzma2.h"

/*-****************************************
 *  error codes list
 *  note : this API is still considered unstable
 *         and shall not be used with a dynamic library.
 *         only static linking is allowed
 ******************************************/
typedef enum {
  UF2_error_no_error                = 0,
  UF2_error_GENERIC                 = 1,
  UF2_error_internal                = 2,
  UF2_error_corruption_detected     = 3,
  UF2_error_checksum_wrong          = 4,
  UF2_error_parameter_unsupported   = 5,
  UF2_error_parameter_outOfBound    = 6,
  UF2_error_lclpMax_exceeded        = 7,
  UF2_error_stage_wrong             = 8,
  UF2_error_init_missing            = 9,
  UF2_error_memory_allocation       = 10,
  UF2_error_dstSize_tooSmall        = 11,
  UF2_error_srcSize_wrong           = 12,
  UF2_error_canceled                = 13,
  UF2_error_buffer                  = 14,
  UF2_error_timedOut                = 15,
  UF2_error_maxCode                 = 20  /* never EVER use this value directly, it can change in future versions! Use UF2_isError() instead */
} UF2_ErrorCode;

/*! UF2_getErrorCode() :
    convert a `size_t` function result into a `UF2_ErrorCode` enum type,
    which can be used to compare with enum list published above */
UF2LIB_API UF2_ErrorCode UF2LIB_CALL UF2_getErrorCode(size_t functionResult);
UF2LIB_API const char* UF2LIB_CALL UF2_getErrorString(UF2_ErrorCode code);   /**< Same as UF2_getErrorName, but using a `UF2_ErrorCode` enum argument */


#if defined (__cplusplus)
}
#endif

#endif /* UF2_ERRORS_H_398273423 */

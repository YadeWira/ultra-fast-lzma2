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

#ifndef UF2POOL_H
#define UF2POOL_H

#if defined (__cplusplus)
extern "C" {
#endif


#include <stddef.h>   /* size_t */

typedef struct UF2POOL_ctx_s UF2POOL_ctx;

/*! UF2POOL_create() :
*  Create a thread pool with at most `numThreads` threads.
* `numThreads` must be at least 1.
* @return : UF2POOL_ctx pointer on success, else NULL.
*/
UF2POOL_ctx *UF2POOL_create(size_t numThreads);


/*! UF2POOL_free() :
Free a thread pool returned by UF2POOL_create().
*/
void UF2POOL_free(UF2POOL_ctx *ctx);

/*! UF2POOL_sizeof() :
return memory usage of pool returned by UF2POOL_create().
*/
size_t UF2POOL_sizeof(UF2POOL_ctx *ctx);

/*! UF2POOL_function :
The function type that can be added to a thread pool.
*/
typedef void(*UF2POOL_function)(void *, ptrdiff_t);

/*! UF2POOL_add() :
Add the job `function(opaque)` to the thread pool.
UF2POOL_addRange adds multiple jobs with size_t parameter from first to less than end.
Possibly blocks until there is room in the queue.
Note : The function may be executed asynchronously, so `opaque` must live until the function has been completed.
*/
void UF2POOL_add(void* ctxVoid, UF2POOL_function function, void *opaque, ptrdiff_t n);
void UF2POOL_addRange(void *ctx, UF2POOL_function function, void *opaque, ptrdiff_t first, ptrdiff_t end);

int UF2POOL_waitAll(void *ctx, unsigned timeout);

size_t UF2POOL_threadsBusy(void *ctx);

#if defined (__cplusplus)
}
#endif

#endif

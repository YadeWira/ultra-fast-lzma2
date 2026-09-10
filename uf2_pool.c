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


/* ======   Dependencies   ======= */
#include <stddef.h>  /* size_t */
#include "uf2_pool.h"
#include "uf2_internal.h"


#ifndef UF2_SINGLETHREAD

#include "uf2_threading.h"   /* pthread adaptation */

struct UF2POOL_ctx_s {
    /* Keep track of the threads */
    size_t numThreads;

    /* All threads work on the same function and object during a job */
    UF2POOL_function function;
    void *opaque;

    /* The number of threads working on jobs */
    size_t numThreadsBusy;
    /* Indicates the number of threads requested and the values to pass */
    ptrdiff_t queueIndex;
    ptrdiff_t queueEnd;

    /* The mutex protects the queue */
    UF2_pthread_mutex_t queueMutex;
    /* Condition variable for pushers to wait on when the queue is full */
    UF2_pthread_cond_t busyCond;
    /* Condition variable for poppers to wait on when the queue is empty */
    UF2_pthread_cond_t newJobsCond;
    /* Indicates if the queue is shutting down */
    int shutdown;

    /* The threads. Extras to be calloc'd */
    UF2_pthread_t threads[1];
};

/* UF2POOL_thread() :
   Work thread for the thread pool.
   Waits for jobs and executes them.
   @returns : NULL on failure else non-null.
*/
static void* UF2POOL_thread(void* opaque)
{
    UF2POOL_ctx* const ctx = (UF2POOL_ctx*)opaque;
    if (!ctx) { return NULL; }
    UF2_pthread_mutex_lock(&ctx->queueMutex);
    for (;;) {

        /* While the mutex is locked, wait for a non-empty queue or until shutdown */
        while (ctx->queueIndex >= ctx->queueEnd && !ctx->shutdown) {
            UF2_pthread_cond_wait(&ctx->newJobsCond, &ctx->queueMutex);
        }
        /* empty => shutting down: so stop */
        if (ctx->shutdown) {
            UF2_pthread_mutex_unlock(&ctx->queueMutex);
            return opaque;
        }
        /* Pop a job off the queue */
        size_t n = ctx->queueIndex;
        ++ctx->queueIndex;
        ++ctx->numThreadsBusy;
        /* Unlock the mutex and run the job */
        UF2_pthread_mutex_unlock(&ctx->queueMutex);

        ctx->function(ctx->opaque, n);

        UF2_pthread_mutex_lock(&ctx->queueMutex);
        --ctx->numThreadsBusy;
        /* Signal the master thread waiting for jobs to complete */
        UF2_pthread_cond_signal(&ctx->busyCond);
    }  /* for (;;) */
    /* Unreachable */
}

UF2POOL_ctx* UF2POOL_create(size_t numThreads)
{
    UF2POOL_ctx* ctx;
    /* Check the parameters */
    if (!numThreads) { return NULL; }
    /* Allocate the context and zero initialize */
    ctx = UF2_calloc(1, sizeof(UF2POOL_ctx) + (numThreads - 1) * sizeof(UF2_pthread_t));
    if (!ctx) { return NULL; }
    /* Initialize the busy count and jobs range */
    ctx->numThreadsBusy = 0;
    ctx->queueIndex = 0;
    ctx->queueEnd = 0;
    (void)UF2_pthread_mutex_init(&ctx->queueMutex, NULL);
    (void)UF2_pthread_cond_init(&ctx->busyCond, NULL);
    (void)UF2_pthread_cond_init(&ctx->newJobsCond, NULL);
    ctx->shutdown = 0;
    ctx->numThreads = 0;
    /* Initialize the threads */
    {   size_t i;
        for (i = 0; i < numThreads; ++i) {
            if (UF2_pthread_create(&ctx->threads[i], NULL, &UF2POOL_thread, ctx)) {
                ctx->numThreads = i;
                UF2POOL_free(ctx);
                return NULL;
        }   }
        ctx->numThreads = numThreads;
    }
    return ctx;
}

/*! UF2POOL_join() :
    Shutdown the queue, wake any sleeping threads, and join all of the threads.
*/
static void UF2POOL_join(UF2POOL_ctx* ctx)
{
    /* Shut down the queue */
    UF2_pthread_mutex_lock(&ctx->queueMutex);
    ctx->shutdown = 1;
    /* Wake up sleeping threads */
    UF2_pthread_cond_broadcast(&ctx->newJobsCond);
    UF2_pthread_mutex_unlock(&ctx->queueMutex);
    /* Join all of the threads */
    for (size_t i = 0; i < ctx->numThreads; ++i)
        UF2_pthread_join(ctx->threads[i], NULL);
}

void UF2POOL_free(UF2POOL_ctx *ctx)
{
    if (!ctx) { return; }
    UF2POOL_join(ctx);
    UF2_pthread_mutex_destroy(&ctx->queueMutex);
    UF2_pthread_cond_destroy(&ctx->busyCond);
    UF2_pthread_cond_destroy(&ctx->newJobsCond);
    UF2_free(ctx);
}

size_t UF2POOL_sizeof(UF2POOL_ctx *ctx)
{
    if (ctx==NULL) return 0;  /* supports sizeof NULL */
    return sizeof(*ctx) + ctx->numThreads * sizeof(UF2_pthread_t);
}

void UF2POOL_addRange(void* ctxVoid, UF2POOL_function function, void *opaque, ptrdiff_t first, ptrdiff_t end)
{
    UF2POOL_ctx* const ctx = (UF2POOL_ctx*)ctxVoid;
    if (!ctx || first == end)
		return; 

    /* Callers always wait for jobs to complete before adding a new set */
    assert(!ctx->numThreadsBusy);

    UF2_pthread_mutex_lock(&ctx->queueMutex);
    ctx->function = function;
    ctx->opaque = opaque;
    ctx->queueIndex = first;
    ctx->queueEnd = end;
    UF2_pthread_cond_broadcast(&ctx->newJobsCond);
    UF2_pthread_mutex_unlock(&ctx->queueMutex);
}

void UF2POOL_add(void* ctxVoid, UF2POOL_function function, void *opaque, ptrdiff_t n)
{
    UF2POOL_addRange(ctxVoid, function, opaque, n, n + 1);
}

int UF2POOL_waitAll(void *ctxVoid, unsigned timeout)
{
    UF2POOL_ctx* const ctx = (UF2POOL_ctx*)ctxVoid;
    if (!ctx || (!ctx->numThreadsBusy && ctx->queueIndex >= ctx->queueEnd) || ctx->shutdown) { return 0; }

    UF2_pthread_mutex_lock(&ctx->queueMutex);
    /* Need to test for ctx->queueIndex < ctx->queueEnd in case not all jobs have started */
    if (timeout != 0) {
        if ((ctx->numThreadsBusy || ctx->queueIndex < ctx->queueEnd) && !ctx->shutdown)
            UF2_pthread_cond_timedwait(&ctx->busyCond, &ctx->queueMutex, timeout);
    }
    else {
        while ((ctx->numThreadsBusy || ctx->queueIndex < ctx->queueEnd) && !ctx->shutdown)
            UF2_pthread_cond_wait(&ctx->busyCond, &ctx->queueMutex);
    }
    UF2_pthread_mutex_unlock(&ctx->queueMutex);
    return ctx->numThreadsBusy && !ctx->shutdown;
}

size_t UF2POOL_threadsBusy(void * ctx)
{
    return ((UF2POOL_ctx*)ctx)->numThreadsBusy;
}

#else

struct UF2POOL_ctx_s {
    int dummy;
};

UF2POOL_ctx* UF2POOL_create(size_t numThreads)
{
    (void)numThreads;
    return NULL;
}

void UF2POOL_free(UF2POOL_ctx *ctx)
{
    UF2_free(ctx);
}

size_t UF2POOL_sizeof(UF2POOL_ctx *ctx)
{
    (void)ctx;
    return 0;
}

void UF2POOL_addRange(void* ctxVoid, UF2POOL_function function, void *opaque, ptrdiff_t first, ptrdiff_t end)
{
    (void)ctxVoid;
    (void)function;
    (void)opaque;
    (void)first;
    (void)end;
}

void UF2POOL_add(void* ctxVoid, UF2POOL_function function, void *opaque, ptrdiff_t n)
{
    UF2POOL_addRange(ctxVoid, function, opaque, n, n + 1);
}

int UF2POOL_waitAll(void *ctxVoid, unsigned timeout)
{
    (void)ctxVoid;
    (void)timeout;
    return 0;
}

size_t UF2POOL_threadsBusy(void * ctx)
{
    (void)ctx;
    return 0;
}

#endif  /* UF2_SINGLETHREAD */

/*
 * Copyright (c) 2018 Conor McCarthy
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 *
 */

#ifndef UF2_ATOMIC_H
#define UF2_ATOMIC_H

#if defined (__cplusplus)
extern "C" {
#endif

/* atomic add */

#if !defined(UF2_SINGLETHREAD) && defined(_WIN32)

#ifdef WINVER
#undef WINVER
#endif
#define WINVER       0x0600

#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0600

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>


typedef LONG volatile UF2_atomic;
#define ATOMIC_INITIAL_VALUE -1
#define UF2_atomic_increment(n) InterlockedIncrement(&n)
#define UF2_atomic_add(n, a) InterlockedAdd(&n, a)
#define UF2_nonAtomic_increment(n) (++n)

#elif !defined(UF2_SINGLETHREAD) && defined(__GNUC__)

typedef long UF2_atomic;
#define ATOMIC_INITIAL_VALUE 0
#define UF2_atomic_increment(n) __sync_fetch_and_add(&n, 1)
#define UF2_atomic_add(n, a) __sync_fetch_and_add(&n, a)
#define UF2_nonAtomic_increment(n) (n++)

#elif !defined(UF2_SINGLETHREAD) && defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) && !defined(__STDC_NO_ATOMICS__) /* C11 */

#include <stdatomic.h>

typedef _Atomic long UF2_atomic;
#define ATOMIC_INITIAL_VALUE 0
#define UF2_atomic_increment(n) atomic_fetch_add(&n, 1)
#define UF2_atomic_add(n, a) atomic_fetch_add(&n, a)
#define UF2_nonAtomic_increment(n) (n++)

#else  /* No atomics */

#	ifndef UF2_SINGLETHREAD
#		error  No atomic operations available. Change compiler config or define UF2_SINGLETHREAD for the entire build.
#	endif

typedef long UF2_atomic;
#define ATOMIC_INITIAL_VALUE 0
#define UF2_atomic_increment(n) (n++)
#define UF2_atomic_add(n, a) (n += (a))
#define UF2_nonAtomic_increment(n) (n++)

#endif /* UF2_SINGLETHREAD */


#if defined (__cplusplus)
}
#endif

#endif /* UF2_ATOMIC_H */

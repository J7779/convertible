#ifndef THREADS_H_
#define THREADS_H_
/*****************************************************************************
 * This file is part of Kvazaar HEVC encoder.
 *
 * Copyright (c) 2021, Tampere University, ITU/ISO/IEC, project contributors
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 * 
 * * Neither the name of the Tampere University or ITU/ISO/IEC nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * INCLUDING NEGLIGENCE OR OTHERWISE ARISING IN ANY WAY OUT OF THE USE OF THIS
 ****************************************************************************/

/**
 * \ingroup Threading
 * \file
 * Single-threaded stand-ins for the threading primitives.
 *
 * Modified for Convertible: the encoder is always configured with
 * `threads = 0` and `owf = 0`, in which case kvazaar runs every job in the
 * calling thread (see kvz_threadqueue_submit). The primitives below
 * therefore have nothing to synchronise: locks, condition variables and
 * semaphores are no-ops, atomics are plain arithmetic, and creating a thread
 * fails. The original POSIX names are prefixed with `kvz_` so that they can
 * never collide with a platform's own pthread declarations. Nothing here
 * needs threads, pthreads or OS headers, so the library builds for
 * WebAssembly without threads and for every other target alike.
 */

#include "global.h" // IWYU pragma: keep

#include <stddef.h>
#include <stdint.h>

typedef struct { int unused; } kvz_pthread_mutex_t;
typedef struct { int unused; } kvz_pthread_cond_t;
typedef struct { int unused; } kvz_pthread_rwlock_t;
typedef struct { int unused; } kvz_pthread_attr_t;
typedef struct { int unused; } kvz_pthread_t;
typedef int kvz_sem_t;

static INLINE int kvz_pthread_mutex_init(kvz_pthread_mutex_t *m, const void *attr) { (void)attr; m->unused = 0; return 0; }
static INLINE int kvz_pthread_mutex_destroy(kvz_pthread_mutex_t *m) { (void)m; return 0; }
static INLINE int kvz_pthread_mutex_lock(kvz_pthread_mutex_t *m) { (void)m; return 0; }
static INLINE int kvz_pthread_mutex_unlock(kvz_pthread_mutex_t *m) { (void)m; return 0; }

static INLINE int kvz_pthread_cond_init(kvz_pthread_cond_t *c, const void *attr) { (void)attr; c->unused = 0; return 0; }
static INLINE int kvz_pthread_cond_destroy(kvz_pthread_cond_t *c) { (void)c; return 0; }
static INLINE int kvz_pthread_cond_signal(kvz_pthread_cond_t *c) { (void)c; return 0; }
static INLINE int kvz_pthread_cond_broadcast(kvz_pthread_cond_t *c) { (void)c; return 0; }
static INLINE int kvz_pthread_cond_wait(kvz_pthread_cond_t *c, kvz_pthread_mutex_t *m) { (void)c; (void)m; return 0; }
static INLINE int kvz_pthread_cond_timedwait(kvz_pthread_cond_t *c, kvz_pthread_mutex_t *m, const void *t) { (void)c; (void)m; (void)t; return 0; }

static INLINE int kvz_pthread_rwlock_init(kvz_pthread_rwlock_t *l, const void *attr) { (void)attr; l->unused = 0; return 0; }
static INLINE int kvz_pthread_rwlock_destroy(kvz_pthread_rwlock_t *l) { (void)l; return 0; }
static INLINE int kvz_pthread_rwlock_rdlock(kvz_pthread_rwlock_t *l) { (void)l; return 0; }
static INLINE int kvz_pthread_rwlock_wrlock(kvz_pthread_rwlock_t *l) { (void)l; return 0; }
static INLINE int kvz_pthread_rwlock_unlock(kvz_pthread_rwlock_t *l) { (void)l; return 0; }

static INLINE int kvz_pthread_attr_init(kvz_pthread_attr_t *a) { a->unused = 0; return 0; }
static INLINE int kvz_pthread_attr_destroy(kvz_pthread_attr_t *a) { (void)a; return 0; }
static INLINE int kvz_pthread_attr_getstacksize(const kvz_pthread_attr_t *a, size_t *size) { (void)a; *size = 1024 * 1024; return 0; }
static INLINE int kvz_pthread_attr_setstacksize(kvz_pthread_attr_t *a, size_t size) { (void)a; (void)size; return 0; }

/* Threads are never requested (threads = 0); report failure if they are. */
static INLINE int kvz_pthread_create(kvz_pthread_t *t, const kvz_pthread_attr_t *a, void *(*fn)(void *), void *arg)
{ (void)t; (void)a; (void)fn; (void)arg; return -1; }
static INLINE int kvz_pthread_join(kvz_pthread_t t, void **ret) { (void)t; if (ret) *ret = NULL; return 0; }

static INLINE void kvz_sem_init(kvz_sem_t *sem, int value) { *sem = value; }
static INLINE void kvz_sem_wait(kvz_sem_t *sem) { --*sem; }
static INLINE void kvz_sem_post(kvz_sem_t *sem) { ++*sem; }
static INLINE void kvz_sem_destroy(kvz_sem_t *sem) { (void)sem; }

/* Single-threaded: plain increments, returning the new value. */
static INLINE int32_t kvz_atomic_add(volatile int32_t *p, int32_t d) { *p += d; return *p; }
#define KVZ_ATOMIC_INC(ptr) kvz_atomic_add((volatile int32_t*)(ptr), 1)
#define KVZ_ATOMIC_DEC(ptr) kvz_atomic_add((volatile int32_t*)(ptr), -1)

/* Timing is only used for statistics; no clock is ever read. */
typedef struct { int64_t unused; } kvz_clock_t;
#define KVZ_CLOCK_T kvz_clock_t
#define KVZ_GET_TIME(clock_t) { (clock_t)->unused = 0; }
#define KVZ_CLOCK_T_AS_DOUBLE(ts) (0.0)
#define KVZ_CLOCK_T_DIFF(start, stop) (0.0)

#endif //THREADS_H_

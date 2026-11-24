/******************************************************************************
*
* Copyright (C) 2012 Ittiam Systems Pvt Ltd, Bangalore
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at:
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
******************************************************************************/
/*
 * Single-threaded replacement for common/ithread.c (the interface is
 * common/ithread.h, unchanged).
 *
 * The decoder is always created with one core (u4_num_cores = 1), in which
 * case it never starts a worker thread: parsing and reconstruction run in the
 * calling thread. Mutexes, semaphores and condition variables therefore have
 * nothing to protect and are no-ops, so the library needs no thread support
 * at all (WebAssembly without threads, any platform without pthreads).
 * Thread creation reports failure; it is unreachable with one core.
 */
#include <string.h>
#include "ihevc_typedefs.h"
#include "ithread.h"

typedef struct
{
    WORD32 i4_unused;
} ithread_single_object_t;

UWORD32 ithread_get_handle_size(void)
{
    return sizeof(ithread_single_object_t);
}

UWORD32 ithread_get_mutex_lock_size(void)
{
    return sizeof(ithread_single_object_t);
}

WORD32 ithread_create(void *thread_handle, void *attribute, void *strt, void *argument)
{
    (void)thread_handle;
    (void)attribute;
    (void)strt;
    (void)argument;
    return -1;
}

WORD32 ithread_join(void *thread_handle, void **val_ptr)
{
    (void)thread_handle;
    if(val_ptr)
        *val_ptr = NULL;
    return 0;
}

void ithread_exit(void *val_ptr)
{
    (void)val_ptr;
}

WORD32 ithread_get_mutex_struct_size(void)
{
    return (WORD32)sizeof(ithread_single_object_t);
}

WORD32 ithread_mutex_init(void *mutex)
{
    if(mutex)
        memset(mutex, 0, sizeof(ithread_single_object_t));
    return 0;
}

WORD32 ithread_mutex_destroy(void *mutex)
{
    (void)mutex;
    return 0;
}

WORD32 ithread_mutex_lock(void *mutex)
{
    (void)mutex;
    return 0;
}

WORD32 ithread_mutex_unlock(void *mutex)
{
    (void)mutex;
    return 0;
}

void ithread_yield(void)
{
}

void ithread_sleep(UWORD32 u4_time)
{
    (void)u4_time;
}

void ithread_msleep(UWORD32 u4_time_ms)
{
    (void)u4_time_ms;
}

void ithread_usleep(UWORD32 u4_time_us)
{
    (void)u4_time_us;
}

UWORD32 ithread_get_sem_struct_size(void)
{
    return sizeof(ithread_single_object_t);
}

WORD32 ithread_sem_init(void *sem, WORD32 pshared, UWORD32 value)
{
    (void)pshared;
    (void)value;
    if(sem)
        memset(sem, 0, sizeof(ithread_single_object_t));
    return 0;
}

WORD32 ithread_sem_post(void *sem)
{
    (void)sem;
    return 0;
}

WORD32 ithread_sem_wait(void *sem)
{
    (void)sem;
    return 0;
}

WORD32 ithread_sem_destroy(void *sem)
{
    (void)sem;
    return 0;
}

WORD32 ithread_set_affinity(WORD32 core_id)
{
    (void)core_id;
    return 1;
}

WORD32 ithread_get_cond_struct_size(void)
{
    return (WORD32)sizeof(ithread_single_object_t);
}

WORD32 ithread_cond_init(void *cond)
{
    if(cond)
        memset(cond, 0, sizeof(ithread_single_object_t));
    return 0;
}

WORD32 ithread_cond_destroy(void *cond)
{
    (void)cond;
    return 0;
}

WORD32 ithread_cond_wait(void *cond, void *mutex)
{
    (void)cond;
    (void)mutex;
    return 0;
}

WORD32 ithread_cond_signal(void *cond)
{
    (void)cond;
    return 0;
}

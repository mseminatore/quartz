//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Mutex API (non-recursive). Implements priority inheritance with full
// multi-mutex tracking: a task that holds several mutexes keeps any boost
// until every boosting waiter has been satisfied (or has timed out).
//---------------------------------------------------------------------------
#ifndef RTOS_MUTEX_H
#define RTOS_MUTEX_H

#include <stdint.h>
#include "rtos_config.h"
#include "rtos_task.h"

// Mutex storage — declare as a static variable and pass its address.
typedef struct rtos_mutex {
    rtos_tcb_t         *owner;       // task currently holding the mutex (NULL if free)
    rtos_tcb_t         *wait_list;   // tasks blocked waiting to acquire
#if RTOS_ENABLE_PRIORITY_INHERITANCE
    struct rtos_mutex  *next_held;   // intrusive link in owner->held_mutexes list
#endif
#if RTOS_ENABLE_RECURSIVE_MUTEX
    uint8_t             recursive;   // 0 = standard, 1 = recursive
    uint8_t             nest_count;  // recursion depth (recursive mutex only)
#endif
} rtos_mutex_t;

rtos_handle_t rtos_mutex_create(rtos_mutex_t *mutex);

#if RTOS_ENABLE_RECURSIVE_MUTEX
// Create a recursive mutex. The same task may lock it multiple times; it must
// be unlocked the same number of times before another task can acquire it.
rtos_handle_t rtos_mutex_create_recursive(rtos_mutex_t *mutex);
#endif

// Acquire. Returns RTOS_OK, or RTOS_TIMEOUT if timed out.
int  rtos_mutex_lock(rtos_handle_t mutex, rtos_tick_t timeout_ticks);

// Release. Unblocks the highest-priority waiter if any.
// Returns RTOS_OK on success, RTOS_ERR if the caller is not the owner.
int rtos_mutex_unlock(rtos_handle_t mutex);

#endif // RTOS_MUTEX_H

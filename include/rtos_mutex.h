//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Mutex API (non-recursive).
//---------------------------------------------------------------------------
#ifndef RTOS_MUTEX_H
#define RTOS_MUTEX_H

#include <stdint.h>
#include "rtos_config.h"
#include "rtos_task.h"

// Mutex storage — declare as a static variable and pass its address.
typedef struct {
    rtos_tcb_t  *owner;       // task currently holding the mutex (NULL if free)
    rtos_tcb_t  *wait_list;   // tasks blocked waiting to acquire
} rtos_mutex_t;

rtos_handle_t rtos_mutex_create(rtos_mutex_t *mutex);

// Acquire. Returns RTOS_OK, or RTOS_TIMEOUT if timed out.
int  rtos_mutex_lock(rtos_handle_t mutex, uint32_t timeout_ticks);

// Release. Unblocks the highest-priority waiter if any.
// Returns RTOS_OK on success, RTOS_ERR if the caller is not the owner.
int rtos_mutex_unlock(rtos_handle_t mutex);

#endif // RTOS_MUTEX_H

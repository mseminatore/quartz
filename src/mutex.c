//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Mutex implementation (non-recursive).
//---------------------------------------------------------------------------
#include <stdint.h>
#include "../include/rtos_mutex.h"
#include "../include/rtos_trace.h"
#include "list.h"
#include "port.h"

extern rtos_tcb_t  *rtos_next_task(void);
extern rtos_tcb_t **rtos_current_tcb_ptr(void);
extern void         rtos_task_make_ready(rtos_tcb_t *tcb);
extern void         rtos_task_blocked_add(rtos_tcb_t *tcb, uint32_t timeout_ticks);
extern void         rtos_task_blocked_remove(rtos_tcb_t *tcb);
extern void         ready_add(rtos_tcb_t *tcb);
extern void         ready_remove(rtos_tcb_t *tcb);

#define current_task() (*rtos_current_tcb_ptr())

//---------------------------------------------------------------------------
// Create a mutex. The caller must provide storage for the mutex struct, which
// can be on the caller's stack or in static memory. Returns a handle to the mutex,
// or NULL on failure. The mutex is created in the unlocked state.
//---------------------------------------------------------------------------
rtos_handle_t rtos_mutex_create(rtos_mutex_t *mutex)
{
    if (!mutex) return NULL;
    mutex->owner     = NULL;
    mutex->wait_list = NULL;
    return (rtos_handle_t)mutex;
}

//---------------------------------------------------------------------------
// Lock a mutex. If the mutex is unlocked, locks it and returns OK. If the 
// mutex is already locked by another task, blocks the current task until 
// either the mutex is unlocked (in which case the task acquires the mutex 
// and returns OK) or the timeout expires (in which case the task returns 
// TIMEOUT). If timeout_ticks is RTOS_NO_WAIT, do not block and return TIMEOUT 
// immediately if the mutex is already locked.
//---------------------------------------------------------------------------
int rtos_mutex_lock(rtos_handle_t handle, uint32_t timeout_ticks)
{
    rtos_mutex_t *mutex = (rtos_mutex_t *)handle;
    if (!mutex) return RTOS_ERR;

    port_enter_critical();

    if (!mutex->owner) 
    {
        mutex->owner = current_task();
        port_exit_critical();
        RTOS_TRACE_MUTEX_LOCK(mutex);
        return RTOS_OK;
    }

    if (timeout_ticks == RTOS_NO_WAIT) 
    {
        port_exit_critical();
        return RTOS_TIMEOUT;
    }

    // Block current task on the mutex wait list and the tick-wakeup list
    rtos_tcb_t *self  = current_task();
    rtos_tcb_t *owner = mutex->owner;
    self->state = TASK_BLOCKED;
    self->ipc_wait = &mutex->wait_list;
    list_insert_sorted(&mutex->wait_list, self, self->priority);
    rtos_task_blocked_add(self, timeout_ticks);

    // Priority inheritance: if the owner has lower priority (higher number),
    // boost it so it can release the mutex sooner (single-level, no chain).
    if (owner && owner->priority > self->priority) {
        if (owner->state == TASK_READY) {
            ready_remove(owner);
            owner->priority = self->priority;
            ready_add(owner);
        } else {
            // TASK_RUNNING or TASK_BLOCKED: just update priority.
            // Running: takes effect at next context switch.
            // Blocked: its position on the IPC list is by key, unchanged.
            owner->priority = self->priority;
        }
    }

    port_exit_critical();

    port_request_reschedule();

    port_enter_critical();
    int on_list = list_remove(&mutex->wait_list, self);
    if (on_list) rtos_task_blocked_remove(self);
    self->ipc_wait = NULL;
    port_exit_critical();

    return on_list ? RTOS_TIMEOUT : (RTOS_TRACE_MUTEX_LOCK(mutex), RTOS_OK);
}

//---------------------------------------------------------------------------
// Unlock a mutex. If there are tasks blocked waiting for the mutex, unblocks
// the highest-priority one and gives it the mutex. Otherwise, sets the mutex to
// the unlocked state. The caller must be the task currently holding the mutex;
// behavior is undefined if this is not the case.
//---------------------------------------------------------------------------
void rtos_mutex_unlock(rtos_handle_t handle)
{
    rtos_mutex_t *mutex = (rtos_mutex_t *)handle;
    if (!mutex) return;

    port_enter_critical();

    // Restore inherited priority before transferring ownership.
    rtos_tcb_t *self = mutex->owner;
    if (self && self->priority != self->base_priority)
        self->priority = self->base_priority;

    rtos_tcb_t *waiter = list_pop_head(&mutex->wait_list);
    if (waiter) 
    {
        mutex->owner = waiter;
        rtos_task_make_ready(waiter);
    } else 
    {
        mutex->owner = NULL;
    }

    port_exit_critical();
    RTOS_TRACE_MUTEX_UNLOCK(mutex);
    port_request_reschedule();
}

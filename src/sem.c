//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Semaphore implementation (binary and counting).
//---------------------------------------------------------------------------
#include <stdint.h>
#include "../include/rtos_sem.h"
#include "../include/rtos_trace.h"
#include "list.h"
#include "port.h"

extern rtos_tcb_t  *rtos_next_task(void);
extern rtos_tcb_t **rtos_current_tcb_ptr(void);
extern void         rtos_task_make_ready(rtos_tcb_t *tcb);
extern void         rtos_task_blocked_add(rtos_tcb_t *tcb, rtos_tick_t timeout_ticks);
extern void         rtos_task_blocked_remove(rtos_tcb_t *tcb);

#define current_task() (*rtos_current_tcb_ptr())

//---------------------------------------------------------------------------
// Create a binary semaphore. The caller must provide storage for the semaphore
// struct, which can be on the caller's stack or in static memory. Returns a handle
// to the semaphore, or NULL on failure. The semaphore is created with count 0 (empty).
//---------------------------------------------------------------------------
rtos_handle_t rtos_semaphore_create_binary(rtos_sem_t *sem)
{
    if (!sem) return NULL;

    sem->count     = 0;
    sem->max_count = 1;
    sem->wait_list = NULL;

    return (rtos_handle_t)sem;
}

//---------------------------------------------------------------------------
// Create a counting semaphore. The caller must provide storage for the semaphore
// struct, which can be on the caller's stack or in static memory. Returns a handle
// to the semaphore, or NULL on failure (e.g. invalid parameters). The semaphore is
// created with the specified initial count.
//---------------------------------------------------------------------------
rtos_handle_t rtos_semaphore_create_counting(rtos_sem_t *sem,
                                              uint32_t    max_count,
                                              uint32_t    initial_count)
{
    if (!sem || max_count == 0 || initial_count > max_count) return NULL;

    sem->count     = initial_count;
    sem->max_count = max_count;
    sem->wait_list = NULL;

    return (rtos_handle_t)sem;
}

//---------------------------------------------------------------------------
// Take a semaphore. If the semaphore count is >0, decrement it and return OK.
// If the count is 0, block the current task until either the semaphore is given
// (in which case the count is not incremented, but the task is unblocked and
// returns OK) or the timeout expires (in which case the task is unblocked and
// returns TIMEOUT). If timeout_ticks is RTOS_NO_WAIT, do not block and return
// TIMEOUT immediately if the semaphore is not available.
//---------------------------------------------------------------------------
int rtos_semaphore_take(rtos_handle_t handle, rtos_tick_t timeout_ticks)
{
    rtos_sem_t *sem = (rtos_sem_t *)handle;
    if (!sem) return RTOS_ERR;

    port_enter_critical();

    if (sem->count > 0) {
        sem->count--;
        port_exit_critical();
        RTOS_TRACE_SEM_TAKE(sem);
        return RTOS_OK;
    }

    // Semaphore not available
    if (timeout_ticks == RTOS_NO_WAIT) {
        port_exit_critical();
        return RTOS_TIMEOUT;
    }

    // Block the current task on the IPC wait list and the tick-wakeup list
    rtos_tcb_t *self = current_task();
    self->state = TASK_BLOCKED;
    self->ipc_wait = &sem->wait_list;
    list_insert_sorted(&sem->wait_list, self, self->priority);
    rtos_task_blocked_add(self, timeout_ticks);
    port_exit_critical();

    port_request_reschedule();

    // If we were given the semaphore, we were popped from wait_list by give().
    // If we timed out, we are still on wait_list and were popped from G_BLOCKED
    // by the tick handler. Clean up the list we're still on.
    // If a notification woke us, we were removed from wait_list by
    // rtos_task_notify — detect this via notif_pending and treat as timeout.
    port_enter_critical();
    int on_list = list_remove(&sem->wait_list, self);
    if (on_list) rtos_task_blocked_remove(self);
    int notified = self->notif_pending;
    self->ipc_wait = NULL;
    port_exit_critical();

    if (on_list || notified) return RTOS_TIMEOUT;
    RTOS_TRACE_SEM_TAKE(sem);
    return RTOS_OK;
}

//---------------------------------------------------------------------------
// Give a semaphore. If there are tasks blocked waiting for the semaphore, unblock
// the highest-priority one and do not increment the count. Otherwise, if the count
// is less than max_count, increment it. If the count is already at max_count,
// does nothing. Unblocking a task may cause a context switch if the unblocked task
// has higher priority than the current task.
//---------------------------------------------------------------------------
void rtos_semaphore_give(rtos_handle_t handle)
{
    rtos_sem_t *sem = (rtos_sem_t *)handle;
    if (!sem) return;

    port_enter_critical();

    rtos_tcb_t *waiter = list_pop_head(&sem->wait_list);
    if (waiter) 
    {
        // Hand the token directly to the waiter — do not increment count.
        rtos_task_make_ready(waiter);
    } else if (sem->count < sem->max_count) 
    {
        sem->count++;
    }

    port_exit_critical();
    RTOS_TRACE_SEM_GIVE(sem);
    port_request_reschedule();
}

//---------------------------------------------------------------------------
// Give a semaphore from an ISR. Same behavior as xSemaphoreGive(), but does 
// not call port_request_reschedule() — the caller is responsible for triggering 
// a reschedule if a higher-priority task was unblocked.
//---------------------------------------------------------------------------
void rtos_semaphore_give_from_isr(rtos_handle_t handle)
{
    rtos_sem_t *sem = (rtos_sem_t *)handle;
    if (!sem) return;

    rtos_tcb_t *waiter = list_pop_head(&sem->wait_list);
    if (waiter) {
        rtos_task_make_ready(waiter);
    } else if (sem->count < sem->max_count) {
        sem->count++;
    }
    // Caller is responsible for triggering a reschedule via port_request_reschedule()
    // if a higher-priority task was unblocked.
}

//---------------------------------------------------------------------------
// Take a semaphore from an ISR. Non-blocking: returns RTOS_OK and decrements
// the count if a token is available, RTOS_ERR if the semaphore is empty.
// Does not reschedule; call port_request_reschedule() after if needed.
//---------------------------------------------------------------------------
int rtos_semaphore_take_from_isr(rtos_handle_t handle)
{
    rtos_sem_t *sem = (rtos_sem_t *)handle;
    if (!sem) return RTOS_ERR;

    if (sem->count > 0) {
        sem->count--;
        return RTOS_OK;
    }
    return RTOS_ERR;
}

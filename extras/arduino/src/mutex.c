//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Mutex implementation (non-recursive).
//---------------------------------------------------------------------------
#include <stdint.h>
#include "rtos_mutex.h"
#include "rtos_trace.h"
#include "list.h"
#include "port.h"

extern rtos_tcb_t  *rtos_next_task(void);
extern rtos_tcb_t **rtos_current_tcb_ptr(void);
extern void         rtos_task_make_ready(rtos_tcb_t *tcb);
extern void         rtos_task_blocked_add(rtos_tcb_t *tcb, rtos_tick_t timeout_ticks);
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
#if RTOS_ENABLE_PRIORITY_INHERITANCE
    mutex->next_held = NULL;
#endif
#if RTOS_ENABLE_RECURSIVE_MUTEX
    mutex->recursive  = 0;
    mutex->nest_count = 0;
#endif
    return (rtos_handle_t)mutex;
}

#if RTOS_ENABLE_RECURSIVE_MUTEX
//---------------------------------------------------------------------------
// Create a recursive mutex. Same task may lock multiple times; must unlock
// the same number of times before another task can acquire it.
//---------------------------------------------------------------------------
rtos_handle_t rtos_mutex_create_recursive(rtos_mutex_t *mutex)
{
    rtos_handle_t h = rtos_mutex_create(mutex);
    if (h) mutex->recursive = 1;
    return h;
}
#endif

#if RTOS_ENABLE_PRIORITY_INHERITANCE
//---------------------------------------------------------------------------
// Push the mutex onto the owner's intrusive held-mutex list. Caller must
// hold the kernel critical section.
//---------------------------------------------------------------------------
static void held_push(rtos_tcb_t *owner, rtos_mutex_t *mutex)
{
    mutex->next_held    = owner->held_mutexes;
    owner->held_mutexes = mutex;
}

//---------------------------------------------------------------------------
// Remove the mutex from the owner's held-mutex list. Caller must hold the
// kernel critical section.
//---------------------------------------------------------------------------
static void held_remove(rtos_tcb_t *owner, rtos_mutex_t *mutex)
{
    rtos_mutex_t **pp = &owner->held_mutexes;
    while (*pp) {
        if (*pp == mutex) {
            *pp = mutex->next_held;
            mutex->next_held = NULL;
            return;
        }
        pp = &(*pp)->next_held;
    }
}

//---------------------------------------------------------------------------
// Compute the effective priority a task should have given its base priority
// and the highest-priority waiter across every mutex it currently holds.
// Lower numeric value = higher priority.
//---------------------------------------------------------------------------
static uint8_t recompute_effective_priority(rtos_tcb_t *task)
{
    uint8_t       eff = task->base_priority;
    rtos_mutex_t *m   = task->held_mutexes;
    while (m) {
        if (m->wait_list && m->wait_list->priority < eff)
            eff = m->wait_list->priority;
        m = m->next_held;
    }
    return eff;
}

//---------------------------------------------------------------------------
// Apply a freshly-computed effective priority to a task, fixing up the
// per-priority ready queue when needed. Caller must hold the kernel critical
// section.
//---------------------------------------------------------------------------
static void apply_effective_priority(rtos_tcb_t *task, uint8_t new_eff)
{
    if (task->priority == new_eff) return;

    if (task->state == TASK_READY) {
        ready_remove(task);
        task->priority = new_eff;
        ready_add(task);
    } else {
        task->priority = new_eff;
    }
}
#endif // RTOS_ENABLE_PRIORITY_INHERITANCE

//---------------------------------------------------------------------------
// Lock a mutex. If the mutex is unlocked, locks it and returns OK. If the 
// mutex is already locked by another task, blocks the current task until 
// either the mutex is unlocked (in which case the task acquires the mutex 
// and returns OK) or the timeout expires (in which case the task returns 
// TIMEOUT). If timeout_ticks is RTOS_NO_WAIT, do not block and return TIMEOUT 
// immediately if the mutex is already locked.
//---------------------------------------------------------------------------
int rtos_mutex_lock(rtos_handle_t handle, rtos_tick_t timeout_ticks)
{
    rtos_mutex_t *mutex = (rtos_mutex_t *)handle;
    if (!mutex) return RTOS_ERR;

    port_enter_critical();

    if (!mutex->owner) 
    {
        mutex->owner = current_task();
#if RTOS_ENABLE_RECURSIVE_MUTEX
        if (mutex->recursive) mutex->nest_count = 1;
#endif
#if RTOS_ENABLE_PRIORITY_INHERITANCE
        held_push(mutex->owner, mutex);
#endif
        port_exit_critical();
        RTOS_TRACE_MUTEX_LOCK(mutex, 1, 0);
        return RTOS_OK;
    }

#if RTOS_ENABLE_RECURSIVE_MUTEX
    // Recursive re-acquire by the current owner: just bump the nest count.
    if (mutex->recursive && mutex->owner == current_task()) {
        if (mutex->nest_count == 0xFFu) {
            port_exit_critical();
            return RTOS_ERR;  // would overflow
        }
        mutex->nest_count++;
        uint32_t nest = mutex->nest_count;
        port_exit_critical();
        RTOS_TRACE_MUTEX_LOCK(mutex, nest, 0);
        return RTOS_OK;
    }
#endif

    if (timeout_ticks == RTOS_NO_WAIT) 
    {
        port_exit_critical();
        return RTOS_TIMEOUT;
    }

    // Block current task on the mutex wait list and the tick-wakeup list
    rtos_tcb_t *self  = current_task();
    self->state = TASK_BLOCKED;
    self->ipc_wait = &mutex->wait_list;
    list_insert_sorted(&mutex->wait_list, self, self->priority);
    rtos_task_blocked_add(self, timeout_ticks);

#if RTOS_ENABLE_PRIORITY_INHERITANCE
    // Priority inheritance: if the owner has lower priority (higher number),
    // boost it so it can release the mutex sooner (single-level, no chain).
    rtos_tcb_t *owner = mutex->owner;
    if (owner && owner->priority > self->priority)
        apply_effective_priority(owner, self->priority);
#endif

    port_exit_critical();

    port_request_reschedule();

    // Clean up: if still on wait_list we timed out; if notif_pending is set
    // a notification woke us rather than the mutex being released to us.
    // Note: notif_pending is intentionally NOT cleared here. The pending flag
    // passes through so a subsequent rtos_task_notify_wait() sees the signal.
    port_enter_critical();
    int on_list = list_remove(&mutex->wait_list, self);
    if (on_list) {
        rtos_task_blocked_remove(self);
#if RTOS_ENABLE_PRIORITY_INHERITANCE
        // We timed out — the owner may have inherited our priority while we
        // waited. Recompute its effective priority now that we're gone so the
        // boost is dropped if no other waiter justifies it.
        if (mutex->owner)
            apply_effective_priority(mutex->owner,
                                     recompute_effective_priority(mutex->owner));
#endif
    }
#if RTOS_ENABLE_TASK_NOTIFY
    int notified = self->notif_pending;
#else
    int notified = 0;
#endif
    self->ipc_wait = NULL;
    port_exit_critical();

    if (on_list || notified) return RTOS_TIMEOUT;
    RTOS_TRACE_MUTEX_LOCK(mutex, 1, 1);
    return RTOS_OK;
}

//---------------------------------------------------------------------------
// Unlock a mutex. If there are tasks blocked waiting for the mutex, unblocks
// the highest-priority one and gives it the mutex. Otherwise, sets the mutex to
// the unlocked state. Returns RTOS_ERR if the caller is not the mutex owner.
//---------------------------------------------------------------------------
int rtos_mutex_unlock(rtos_handle_t handle)
{
    rtos_mutex_t *mutex = (rtos_mutex_t *)handle;
    if (!mutex) return RTOS_ERR;

    port_enter_critical();

    if (mutex->owner != current_task()) {
        port_exit_critical();
        return RTOS_ERR;
    }

    rtos_tcb_t *self = mutex->owner;

#if RTOS_ENABLE_RECURSIVE_MUTEX
    // Recursive unlock: only release on the outermost call.
    if (mutex->recursive) {
        if (mutex->nest_count > 1) {
            mutex->nest_count--;
            uint32_t nest = mutex->nest_count;
            port_exit_critical();
            RTOS_TRACE_MUTEX_UNLOCK(mutex, nest, 0);
            return RTOS_OK;
        }
        mutex->nest_count = 0;
    }
#endif

#if RTOS_ENABLE_PRIORITY_INHERITANCE
    // Drop this mutex from the owner's held-list before recomputing so any
    // waiters still on this mutex don't keep boosting the outgoing owner.
    held_remove(self, mutex);
#endif

    rtos_tcb_t *waiter = list_pop_head(&mutex->wait_list);
    if (waiter)
    {
        mutex->owner = waiter;
#if RTOS_ENABLE_RECURSIVE_MUTEX
        if (mutex->recursive) mutex->nest_count = 1;
#endif
#if RTOS_ENABLE_PRIORITY_INHERITANCE
        held_push(waiter, mutex);
#endif
        rtos_task_make_ready(waiter);
    } else
    {
        mutex->owner = NULL;
    }

#if RTOS_ENABLE_PRIORITY_INHERITANCE
    // Restore outgoing owner's effective priority based on any *other* mutexes
    // it still holds. If none, this drops back to base_priority.
    apply_effective_priority(self, recompute_effective_priority(self));
#endif

    uint32_t woke = (waiter != NULL) ? 1u : 0u;
    port_exit_critical();
    RTOS_TRACE_MUTEX_UNLOCK(mutex, 0, woke);
    port_request_reschedule();
    return RTOS_OK;
}

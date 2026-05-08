//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Event group implementation.
//
// Design notes
// ------------
// The wait list is a priority-sorted intrusive singly-linked list (the same
// list.h helpers used by semaphore/mutex/queue).  When rtos_eventgroup_set()
// walks the list to find satisfied waiters, it does so in priority order so
// high-priority tasks are always woken first.
//
// clear_on_exit ordering
// ~~~~~~~~~~~~~~~~~~~~~~
// When multiple tasks block with overlapping masks and clear_on_exit, the
// clearing is batched: all satisfied tasks are identified against the full
// post-set bits value, their result bits saved, and only then are the
// accumulated clear masks applied.  This ensures every woken task sees the
// same bit snapshot (the value after the set, before any auto-clears).
//
// Result bit communication
// ~~~~~~~~~~~~~~~~~~~~~~~~
// rtos_eventgroup_set() stores the satisfied bits into the blocked task's
// eg_wait_mask field (repurposing it — the original mask is no longer needed
// once the task is unblocked).  rtos_eventgroup_wait() reads this field after
// waking to return the correct value to the caller.
//---------------------------------------------------------------------------

#include <stdint.h>
#include <stddef.h>
#include "../include/rtos_eventgroup.h"
#include "../include/rtos_trace.h"
#include "list.h"
#include "port.h"

#if RTOS_ENABLE_EVENT_GROUPS

extern rtos_tcb_t  *rtos_next_task(void);
extern rtos_tcb_t **rtos_current_tcb_ptr(void);
extern void         rtos_task_make_ready(rtos_tcb_t *tcb);
extern void         rtos_task_blocked_add(rtos_tcb_t *tcb, rtos_tick_t timeout_ticks);
extern void         rtos_task_blocked_remove(rtos_tcb_t *tcb);

#define current_task()  (*rtos_current_tcb_ptr())

//---------------------------------------------------------------------------
// Internal helper: check whether a blocked task's condition is satisfied
// given the current event group bits.
//---------------------------------------------------------------------------
static int eg_satisfied(const rtos_tcb_t *t, uint32_t bits)
{
    if (t->eg_wait_mode == RTOS_EG_WAIT_ALL)
        return (bits & t->eg_wait_mask) == t->eg_wait_mask;
    else
        return (bits & t->eg_wait_mask) != 0;
}

//---------------------------------------------------------------------------
// Internal helper: wake all satisfied waiters.
// Called inside a critical section.  Returns the bits value that was used to
// check conditions (post-set, pre-clear), so the caller can pass it to the
// trace hook.  Updates eg->bits with the accumulated clear masks.
//---------------------------------------------------------------------------
static uint32_t eg_wake_satisfied(rtos_eventgroup_t *eg, uint32_t *tasks_woken_out)
{
    uint32_t snapshot        = eg->bits;   // stable reference for all checks
    uint32_t accumulated_clr = 0;
    uint32_t woken           = 0;

    rtos_tcb_t **pp = &eg->wait_list;
    while (*pp) {
        rtos_tcb_t *t = *pp;
        if (eg_satisfied(t, snapshot)) {
            // Store result bits in eg_wait_mask before overwriting it.
            uint32_t result = snapshot & t->eg_wait_mask;
            accumulated_clr |= t->eg_clear_mask;

            // Splice out of the wait list (without using list_remove O(n²)).
            *pp = t->next;
            t->next = NULL;
            t->ipc_wait = NULL;

            // Reuse eg_wait_mask to pass result bits back to waiting task.
            t->eg_wait_mask = result;

            rtos_task_make_ready(t);   // also removes from g_blocked if present
            woken++;
            // Don't advance pp — *pp is now the next node.
        } else {
            pp = &t->next;
        }
    }

    eg->bits &= ~accumulated_clr;

    if (tasks_woken_out) *tasks_woken_out = woken;
    return snapshot;
}

//---------------------------------------------------------------------------
// Create (initialise) an event group.
//---------------------------------------------------------------------------
rtos_handle_t rtos_eventgroup_create(rtos_eventgroup_t *eg)
{
    if (!eg) return NULL;
    eg->bits      = 0;
    eg->wait_list = NULL;
    return (rtos_handle_t)eg;
}

//---------------------------------------------------------------------------
// Set bits — task context.
//---------------------------------------------------------------------------
uint32_t rtos_eventgroup_set(rtos_handle_t handle, uint32_t bits_to_set)
{
    rtos_eventgroup_t *eg = (rtos_eventgroup_t *)handle;
    if (!eg || !bits_to_set) return eg ? eg->bits : 0;

    port_enter_critical();
    eg->bits |= bits_to_set;
    uint32_t woken;
    uint32_t snapshot = eg_wake_satisfied(eg, &woken);
    uint32_t result   = snapshot;   // value seen by caller (pre-clear)
    port_exit_critical();

    RTOS_TRACE_EG_SET(eg, result, woken);

    if (woken)
        port_request_reschedule();

    return result;
}

//---------------------------------------------------------------------------
// Set bits — ISR context.
//---------------------------------------------------------------------------
uint32_t rtos_eventgroup_set_from_isr(rtos_handle_t handle, uint32_t bits_to_set)
{
    rtos_eventgroup_t *eg = (rtos_eventgroup_t *)handle;
    if (!eg || !bits_to_set) return eg ? eg->bits : 0;

    eg->bits |= bits_to_set;
    uint32_t woken;
    uint32_t snapshot = eg_wake_satisfied(eg, &woken);
    uint32_t result   = snapshot;

    RTOS_TRACE_EG_SET(eg, result, woken);

    if (woken)
        port_request_reschedule();

    return result;
}

//---------------------------------------------------------------------------
// Clear bits.  Returns value before clearing.
//---------------------------------------------------------------------------
uint32_t rtos_eventgroup_clear(rtos_handle_t handle, uint32_t bits_to_clear)
{
    rtos_eventgroup_t *eg = (rtos_eventgroup_t *)handle;
    if (!eg) return 0;

    port_enter_critical();
    uint32_t before = eg->bits;
    eg->bits &= ~bits_to_clear;
    port_exit_critical();
    return before;
}

//---------------------------------------------------------------------------
// Read current bits — no side effects.
//---------------------------------------------------------------------------
uint32_t rtos_eventgroup_get(rtos_handle_t handle)
{
    rtos_eventgroup_t *eg = (rtos_eventgroup_t *)handle;
    if (!eg) return 0;

    port_enter_critical();
    uint32_t bits = eg->bits;
    port_exit_critical();
    return bits;
}

//---------------------------------------------------------------------------
// Wait for bits — task context only.
//---------------------------------------------------------------------------
uint32_t rtos_eventgroup_wait(rtos_handle_t handle,
                               uint32_t      wait_mask,
                               int           wait_all,
                               int           clear_on_exit,
                               rtos_tick_t   timeout_ticks)
{
    rtos_eventgroup_t *eg = (rtos_eventgroup_t *)handle;
    if (!eg || !wait_mask) return 0;

    port_enter_critical();

    // Fast path: condition already satisfied.
    int immediate = (wait_all == RTOS_EG_WAIT_ALL)
                    ? ((eg->bits & wait_mask) == wait_mask)
                    : ((eg->bits & wait_mask) != 0);

    if (immediate) {
        uint32_t result = eg->bits & wait_mask;
        if (clear_on_exit)
            eg->bits &= ~wait_mask;
        port_exit_critical();
        RTOS_TRACE_EG_WAIT(eg, result, 0);
        return result;
    }

    // Condition not yet satisfied.
    if (timeout_ticks == RTOS_NO_WAIT) {
        port_exit_critical();
        return 0;
    }

    // Block until bits are set or timeout expires.
    rtos_tcb_t *self = current_task();
    self->state         = TASK_BLOCKED;
    self->eg_wait_mask  = wait_mask;
    self->eg_clear_mask = clear_on_exit ? wait_mask : 0;
    self->eg_wait_mode  = (uint8_t)(wait_all ? RTOS_EG_WAIT_ALL : RTOS_EG_WAIT_ANY);
    self->ipc_wait      = &eg->wait_list;

    // Insert sorted by priority so the wake scan always processes higher
    // priority tasks first (list_insert_sorted uses tcb->sort_key on insert,
    // but we pass priority explicitly as the key).
    list_insert_sorted(&eg->wait_list, self, self->priority);
    rtos_task_blocked_add(self, timeout_ticks);
    port_exit_critical();

    port_request_reschedule();

    // --- resumed here ---

    // If we timed out, we are still on the wait list but were removed from
    // g_blocked by the tick handler.  Clean up.
    // If we were woken by set(), we were already removed from the wait list
    // and eg_wait_mask holds our result bits.
    port_enter_critical();
    int still_waiting = list_remove(&eg->wait_list, self);
    if (still_waiting) rtos_task_blocked_remove(self);
    uint32_t result = still_waiting ? 0 : self->eg_wait_mask;
    self->ipc_wait  = NULL;
    port_exit_critical();

    RTOS_TRACE_EG_WAIT(eg, result, 1);
    return result;
}

#endif // RTOS_ENABLE_EVENT_GROUPS

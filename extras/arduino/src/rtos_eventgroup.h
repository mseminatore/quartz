//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Event group API.
//
// An event group is a set of 32 binary flags stored in a single kernel
// object.  Tasks can set or clear individual bits and block until any or all
// bits in a mask are set, with optional auto-clear on wake.  Multiple tasks
// may block on the same event group simultaneously.
//
// Typical use-cases:
//   - Fan-in synchronization: wait until event A AND event B AND event C
//     have all been signalled (e.g. all peripherals initialised).
//   - Broadcast: set a flag so every waiter wakes at once (ANY mode,
//     no clear_on_exit so all tasks see the flag).
//   - ISR → task handoff: set_from_isr() is safe to call from interrupt
//     context; wait() may only be called from task context.
//
// Static allocation pattern (zero malloc):
//   static rtos_eventgroup_t g_events;
//   rtos_handle_t h = rtos_eventgroup_create(&g_events);
//
// Compile-time opt-out: set RTOS_ENABLE_EVENT_GROUPS=0 before including
// rtos.h to remove all event group code and the per-TCB overhead (≈12 bytes
// per task on 32-bit targets).
//---------------------------------------------------------------------------
#ifndef RTOS_EVENTGROUP_H
#define RTOS_EVENTGROUP_H

#include <stdint.h>
#include "rtos_config.h"
#include "rtos_task.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RTOS_ENABLE_EVENT_GROUPS

// ---------------------------------------------------------------------------
// Wait-mode constants
// ---------------------------------------------------------------------------

// Unblock when *any* bit in the wait mask is set.
#define RTOS_EG_WAIT_ANY  0

// Unblock only when *all* bits in the wait mask are simultaneously set.
#define RTOS_EG_WAIT_ALL  1

// ---------------------------------------------------------------------------
// Event group storage — declare as a static variable and pass its address.
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t    bits;       // current state of all event bits
    rtos_tcb_t *wait_list;  // tasks blocked waiting for bits (sorted by priority)
} rtos_eventgroup_t;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Create (initialise) an event group.  The caller must provide storage.
// Returns a handle, or NULL if eg is NULL.
rtos_handle_t rtos_eventgroup_create(rtos_eventgroup_t *eg);

// Set one or more bits.  Any tasks whose wait conditions are now satisfied
// are unblocked (in priority order).  If a task has clear_on_exit set, the
// bits it waited on are cleared after it wakes.
//
// Returns the value of the event group bits *after* setting and *before* any
// auto-clears triggered by waking tasks (so the caller sees the full set).
// May only be called from task context.
uint32_t rtos_eventgroup_set(rtos_handle_t eg, uint32_t bits_to_set);

// ISR-safe version of rtos_eventgroup_set.  Does not call
// port_request_reschedule(); the reschedule is requested inside.
uint32_t rtos_eventgroup_set_from_isr(rtos_handle_t eg, uint32_t bits_to_set);

// Clear one or more bits.  Returns the event group value *before* clearing.
// May be called from task or ISR context.
uint32_t rtos_eventgroup_clear(rtos_handle_t eg, uint32_t bits_to_clear);

// Read the current bits without blocking or modifying them.
// May be called from task or ISR context.
uint32_t rtos_eventgroup_get(rtos_handle_t eg);

// Block until the wait condition is satisfied or the timeout expires.
//
//   wait_mask     — set of bits of interest (must be non-zero)
//   wait_all      — RTOS_EG_WAIT_ALL: all bits; RTOS_EG_WAIT_ANY: any bit
//   clear_on_exit — if non-zero, clear wait_mask bits in the event group
//                   when this call returns successfully
//   timeout_ticks — RTOS_WAIT_FOREVER, RTOS_NO_WAIT, or a finite tick count
//
// Returns the event group bits that satisfied the condition (snapshot taken
// before clear_on_exit is applied), or 0 on timeout.
//
// May only be called from task context.
uint32_t rtos_eventgroup_wait(rtos_handle_t eg,
                               uint32_t      wait_mask,
                               int           wait_all,
                               int           clear_on_exit,
                               rtos_tick_t   timeout_ticks);

#endif // RTOS_ENABLE_EVENT_GROUPS

#ifdef __cplusplus
}
#endif

#endif // RTOS_EVENTGROUP_H

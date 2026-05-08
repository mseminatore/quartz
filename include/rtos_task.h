//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Task management API.
//---------------------------------------------------------------------------
#ifndef RTOS_TASK_H
#define RTOS_TASK_H

#include <stdint.h>
#include <stddef.h>
#include "rtos_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *rtos_handle_t;

// Task states
typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_SUSPENDED,
    TASK_DELETED,
} rtos_task_state_t;

// Priority levels (0 = highest, RTOS_MAX_PRIORITIES - 1 = lowest)
enum Priority {
    RTOS_PRIORITY_HIGH = 0,
    RTOS_PRIORITY_MEDIUM = 1,
    RTOS_PRIORITY_LOW = 2,
    RTOS_PRIORITY_IDLE = RTOS_MAX_PRIORITIES - 1,
};

// Task Control Block — storage provided by the user as a static variable
struct rtos_mutex;  // forward decl for held-mutex list

typedef struct rtos_tcb {
    void               *sp;                      // saved stack pointer (port-specific type)
#if RTOS_STACK_OVERFLOW_CHECK || RTOS_STACK_WATERMARK
    void               *stack_base;              // bottom of stack (for overflow detection)
    size_t              stack_words;
#endif
    uint8_t             priority;
#if RTOS_ENABLE_PRIORITY_INHERITANCE
    uint8_t             base_priority;           // original priority before any inheritance boost
    struct rtos_mutex  *held_mutexes;            // intrusive list of mutexes currently owned
#endif
    rtos_task_state_t   state;
    rtos_tick_t         wakeup_tick;             // absolute tick at which task should unblock
    rtos_tick_t         sort_key;                // sort key when on an IPC wait list
    char                name[RTOS_TASK_NAME_LEN];
    struct rtos_tcb    *next;                    // intrusive list link
#if RTOS_ENABLE_TASK_NOTIFY
    uint8_t             notif_pending;           // non-zero if a notification is waiting
    uint32_t            notif_value;             // notification value (set/increment/overwrite)
#endif
    uint8_t             on_blocked;              // non-zero when on the per-core blocked list
    struct rtos_tcb   **ipc_wait;               // pointer to the IPC wait-list head this task is on (NULL if none)
#if RTOS_ENABLE_RUNTIME_STATS
    rtos_tick_t         runtime_ticks;           // total ticks this task has been running
#endif
#if RTOS_NUM_CORES > 1
    uint8_t             core;                    // CPU core this task is pinned to
#endif
#if RTOS_ENABLE_EVENT_GROUPS
    uint32_t            eg_wait_mask;   // bits this task is waiting for; overwritten with result bits on wake
    uint32_t            eg_clear_mask;  // bits to clear in the event group on exit (0 = no clear)
    uint8_t             eg_wait_mode;   // RTOS_EG_WAIT_ANY (0) or RTOS_EG_WAIT_ALL (1)
#endif
} rtos_tcb_t;

// Create a task on the calling core. tcb and stack must be static storage
// provided by the caller. Declare the stack as:
//   static rtos_stack_t my_stack[N];
// stack_words is the number of elements (each RTOS_STACK_BYTES_PER_WORD bytes).
rtos_handle_t rtos_task_create(rtos_tcb_t    *tcb,
                                rtos_stack_t  *stack,
                                size_t         stack_words,
                                void         (*func)(void *),
                                void          *arg,
                                const char    *name,
                                uint8_t        priority);

// Create a task pinned to a specific CPU core. Only available when RTOS_NUM_CORES > 1.
#if RTOS_NUM_CORES > 1
rtos_handle_t rtos_task_create_on_core(rtos_tcb_t    *tcb,
                                        rtos_stack_t  *stack,
                                        size_t         stack_words,
                                        void         (*func)(void *),
                                        void          *arg,
                                        const char    *name,
                                        uint8_t        priority,
                                        uint8_t        core);

// Entry point for core 1. Pass to multicore_launch_core1() before calling
// rtos_start() on core 0. Initialises core 1's SysTick and starts its scheduler.
void rtos_core1_entry(void);
#endif

// Delay the calling task for the given number of ticks.
void rtos_task_delay(rtos_tick_t ticks);

// Delay until an absolute tick deadline. Eliminates period drift for periodic
// tasks. *last_wake_tick should be initialised to rtos_task_tick_count() before
// the first call. On each call it is advanced by period_ticks.
void rtos_task_delay_until(rtos_tick_t *last_wake_tick, rtos_tick_t period_ticks);

// Voluntarily yield the CPU to the next ready task.
void rtos_task_yield(void);

// Suspend / resume a task by handle (pass NULL to target the current task).
#if RTOS_ENABLE_TASK_SUSPEND
void rtos_task_suspend(rtos_handle_t task);
void rtos_task_resume(rtos_handle_t task);
#endif

// Remove a task from all lists. Pass NULL to delete the current task.
#if RTOS_ENABLE_TASK_DELETE
void rtos_task_delete(rtos_handle_t task);
#endif

// Return the current tick count.
rtos_tick_t rtos_task_tick_count(void);

// Return the handle of the calling task. Useful for passing to APIs that
// accept a task handle (e.g. rtos_task_notify) from within the task itself,
// or from callbacks that need to wake a specific task.
rtos_handle_t rtos_task_handle_self(void);

// ---------------------------------------------------------------------------
// Task notifications — per-task lightweight binary semaphore, zero allocation.
// Only available when RTOS_ENABLE_TASK_NOTIFY is non-zero.
// ---------------------------------------------------------------------------
#if RTOS_ENABLE_TASK_NOTIFY

// Send a notification to a task (from task context). If the task is blocked
// waiting for a notification, it is unblocked immediately.
void rtos_task_notify(rtos_handle_t task);

// Send a notification from an ISR. If the target task is blocked waiting for
// a notification, it is made ready and a reschedule is requested.
void rtos_task_notify_from_isr(rtos_handle_t task);

// Wait for a notification. Returns RTOS_OK when notified, RTOS_TIMEOUT if
// the timeout expires before a notification arrives. Pass RTOS_WAIT_FOREVER
// to block indefinitely.
int rtos_task_notify_wait(rtos_tick_t timeout_ticks);

// Clear a pending task notification on the calling task without blocking.
// Useful for draining a stale notification before a fresh wait loop.
void rtos_task_notify_clear(void);

// ---------------------------------------------------------------------------
// Value-passing notifications (FreeRTOS-style). Each task carries a 32-bit
// notification value that the sender can set, OR with bits, increment, or
// overwrite. The waiter can read and selectively clear bits before/after
// receiving the value.
// ---------------------------------------------------------------------------
typedef enum {
    RTOS_NOTIFY_NONE              = 0,  // mark pending; do not touch value
    RTOS_NOTIFY_SET_BITS          = 1,  // value |= bits
    RTOS_NOTIFY_INCREMENT         = 2,  // value++ (counting semantics)
    RTOS_NOTIFY_OVERWRITE         = 3,  // value = bits (always)
    RTOS_NOTIFY_SET_NO_OVERWRITE  = 4   // value = bits only if not pending
} rtos_notify_action_t;

// Send a value-bearing notification to a task. Returns RTOS_OK on success,
// RTOS_ERR if action == SET_NO_OVERWRITE and a notification is already
// pending (the value is left untouched in that case).
int  rtos_task_notify_value(rtos_handle_t task,
                            rtos_notify_action_t action,
                            uint32_t value);

// ISR-safe version of rtos_task_notify_value.
int  rtos_task_notify_value_from_isr(rtos_handle_t task,
                                     rtos_notify_action_t action,
                                     uint32_t value);

// Wait for a value-bearing notification. Before checking for a pending
// notification, value &= ~clear_on_entry. On successful return,
// *value_out (if non-NULL) receives the notification value, then
// value &= ~clear_on_exit. Returns RTOS_OK or RTOS_TIMEOUT.
int rtos_task_notify_wait_value(uint32_t clear_on_entry,
                                uint32_t clear_on_exit,
                                uint32_t *value_out,
                                rtos_tick_t timeout_ticks);

#endif // RTOS_ENABLE_TASK_NOTIFY

// ---------------------------------------------------------------------------
// Task inspection APIs
// ---------------------------------------------------------------------------

// Return the current state of a task.
rtos_task_state_t rtos_task_get_state(rtos_handle_t task);

// Return a pointer to the task's name string (always NUL-terminated).
const char *rtos_task_get_name(rtos_handle_t task);

// Return the current effective priority (may be temporarily boosted by mutex
// inheritance; use base_priority field of the TCB for the original value).
uint8_t rtos_task_get_priority(rtos_handle_t task);

// Change a task's priority. Pass NULL to target the current task.
// new_priority must be in 0 .. RTOS_MAX_PRIORITIES-2 (idle is reserved).
// Returns RTOS_OK on success, RTOS_ERR if new_priority is out of range.
int rtos_task_set_priority(rtos_handle_t task, uint8_t new_priority);

// Check whether a task's stack sentinel has been overwritten.
// Returns RTOS_OK if intact, RTOS_ERR if overflow detected.
// Only meaningful when RTOS_STACK_OVERFLOW_CHECK is non-zero.
int rtos_task_check_stack(rtos_handle_t task);

// Return the number of stack words that have never been written (high-water
// mark). Only meaningful when RTOS_STACK_WATERMARK is non-zero.
uint32_t rtos_task_stack_high_water_mark(rtos_handle_t task);

// ---------------------------------------------------------------------------
// Runtime CPU statistics (only when RTOS_ENABLE_RUNTIME_STATS is non-zero)
// ---------------------------------------------------------------------------

#if RTOS_ENABLE_RUNTIME_STATS
typedef struct {
    const char   *name;
    rtos_tick_t   runtime_ticks;
    uint8_t       percent;          // 0–100 (integer)
} rtos_runtime_stat_t;

// Fill buf[0..n-1] with stats for all live tasks. Returns number of entries
// written. Entries are unsorted; the caller may sort by runtime_ticks.
size_t rtos_task_get_runtime_stats(rtos_runtime_stat_t *buf, size_t n);
#endif

#ifdef __cplusplus
}
#endif

#endif // RTOS_TASK_H

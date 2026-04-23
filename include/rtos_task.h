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
typedef struct rtos_tcb {
    void               *sp;                      // saved stack pointer (port-specific type)
    void               *stack_base;              // bottom of stack (for overflow detection)
    size_t              stack_words;
    uint8_t             priority;
    rtos_task_state_t   state;
    uint32_t            delay_ticks;             // countdown for vTaskDelay
    uint32_t            sort_key;                // sort key when on blocked or wait list
    char                name[RTOS_TASK_NAME_LEN];
    struct rtos_tcb    *next;                    // intrusive list link
    uint8_t             notif_pending;           // non-zero if a notification is waiting
#if RTOS_ENABLE_RUNTIME_STATS
    uint32_t            runtime_ticks;           // total ticks this task has been running
#endif
#if RTOS_NUM_CORES > 1
    uint8_t             core;                    // CPU core this task is pinned to
#endif
} rtos_tcb_t;

// Create a task on the calling core. tcb and stack must be static storage
// provided by the caller. stack_words is the number of RTOS_STACK_BYTES_PER_WORD
// units in the stack buffer.
rtos_handle_t rtos_task_create(rtos_tcb_t *tcb,
                                void       *stack,
                                size_t      stack_words,
                                void      (*func)(void *),
                                void       *arg,
                                const char *name,
                                uint8_t     priority);

// Create a task pinned to a specific CPU core. Only available when RTOS_NUM_CORES > 1.
#if RTOS_NUM_CORES > 1
rtos_handle_t rtos_task_create_on_core(rtos_tcb_t *tcb,
                                        void       *stack,
                                        size_t      stack_words,
                                        void      (*func)(void *),
                                        void       *arg,
                                        const char *name,
                                        uint8_t     priority,
                                        uint8_t     core);

// Entry point for core 1. Pass to multicore_launch_core1() before calling
// rtos_start() on core 0. Initialises core 1's SysTick and starts its scheduler.
void rtos_core1_entry(void);
#endif

// Delay the calling task for the given number of ticks.
void rtos_task_delay(uint32_t ticks);

// Delay until an absolute tick deadline. Eliminates period drift for periodic
// tasks. *last_wake_tick should be initialised to rtos_task_tick_count() before
// the first call. On each call it is advanced by period_ticks.
void rtos_task_delay_until(uint32_t *last_wake_tick, uint32_t period_ticks);

// Voluntarily yield the CPU to the next ready task.
void rtos_task_yield(void);

// Suspend / resume a task by handle (pass NULL to target the current task).
void rtos_task_suspend(rtos_handle_t task);
void rtos_task_resume(rtos_handle_t task);

// Remove a task from all lists. Pass NULL to delete the current task.
void rtos_task_delete(rtos_handle_t task);

// Return the current tick count.
uint32_t rtos_task_tick_count(void);

// ---------------------------------------------------------------------------
// Task notifications — per-task lightweight binary semaphore, zero allocation.
// ---------------------------------------------------------------------------

// Send a notification to a task (from task context). If the task is blocked
// waiting for a notification, it is unblocked immediately.
void rtos_task_notify(rtos_handle_t task);

// Send a notification from an ISR. Does not call port_request_reschedule();
// the caller must trigger a reschedule if the target task has higher priority.
void rtos_task_notify_from_isr(rtos_handle_t task);

// Wait for a notification. Returns RTOS_OK when notified, RTOS_TIMEOUT if
// the timeout expires before a notification arrives. Pass RTOS_WAIT_FOREVER
// to block indefinitely.
int rtos_task_notify_wait(uint32_t timeout_ticks);

// ---------------------------------------------------------------------------
// Debug / instrumentation APIs
// ---------------------------------------------------------------------------

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
    const char *name;
    uint32_t    runtime_ticks;
    uint8_t     percent;          // 0–100 (integer)
} rtos_runtime_stat_t;

// Fill buf[0..n-1] with stats for all live tasks. Returns number of entries
// written. Entries are unsorted; the caller may sort by runtime_ticks.
size_t rtos_task_get_runtime_stats(rtos_runtime_stat_t *buf, size_t n);
#endif

#endif // RTOS_TASK_H

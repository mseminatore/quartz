//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Task management and scheduler.
//---------------------------------------------------------------------------
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../include/rtos_task.h"
#include "../include/rtos_trace.h"
#include "list.h"
#include "port.h"

//[]---------------------------------------------------------------------------[]
// Stack sentinel / watermark constants
//[]---------------------------------------------------------------------------[]

#define STACK_SENTINEL_WORD   0xDEADBEEFu
#define STACK_SENTINEL_COUNT  4               // words filled at bottom of stack
#define STACK_WATERMARK_WORD  0xA5A5A5A5u

//[]---------------------------------------------------------------------------[]
// Scheduler state — per-core when RTOS_NUM_CORES > 1
//[]---------------------------------------------------------------------------[]

#if RTOS_NUM_CORES > 1

static rtos_tcb_t  *g_ready[RTOS_NUM_CORES][RTOS_MAX_PRIORITIES];
static uint32_t     g_ready_bitmap[RTOS_NUM_CORES];
static rtos_tcb_t  *g_blocked[RTOS_NUM_CORES];
rtos_tcb_t         *g_current[RTOS_NUM_CORES];
static uint32_t     g_tick_count[RTOS_NUM_CORES];

static rtos_tcb_t   g_idle_tcb[RTOS_NUM_CORES];
static uint8_t      g_idle_stack[RTOS_NUM_CORES][RTOS_IDLE_STACK_WORDS * RTOS_STACK_BYTES_PER_WORD];

// Convenience macros — index by current core
#define CORE            port_core_id()
#define G_READY         g_ready[CORE]
#define G_READY_BITMAP  g_ready_bitmap[CORE]
#define G_BLOCKED       g_blocked[CORE]
#define G_CURRENT       g_current[CORE]
#define G_TICK_COUNT    g_tick_count[CORE]

#else  // single-core

static rtos_tcb_t  *g_ready[RTOS_MAX_PRIORITIES];
static uint32_t     g_ready_bitmap;
static rtos_tcb_t  *g_blocked;
rtos_tcb_t         *g_current;
static uint32_t     g_tick_count;

static rtos_tcb_t   g_idle_tcb;
static uint8_t      g_idle_stack[RTOS_IDLE_STACK_WORDS * RTOS_STACK_BYTES_PER_WORD];

#define G_READY         g_ready
#define G_READY_BITMAP  g_ready_bitmap
#define G_BLOCKED       g_blocked
#define G_CURRENT       g_current
#define G_TICK_COUNT    g_tick_count

#endif  // RTOS_NUM_CORES

// ---------------------------------------------------------------------------
// All-tasks list for debug inspection and runtime stats
// ---------------------------------------------------------------------------

#if RTOS_ENABLE_RUNTIME_STATS || RTOS_STACK_OVERFLOW_CHECK
static rtos_tcb_t *g_all_tasks[RTOS_MAX_TASKS];
static size_t      g_all_tasks_count = 0;

static void all_tasks_add(rtos_tcb_t *tcb)
{
    if (g_all_tasks_count < RTOS_MAX_TASKS)
        g_all_tasks[g_all_tasks_count++] = tcb;
}

static void all_tasks_remove(rtos_tcb_t *tcb)
{
    for (size_t i = 0; i < g_all_tasks_count; i++) {
        if (g_all_tasks[i] == tcb) {
            g_all_tasks[i] = g_all_tasks[--g_all_tasks_count];
            return;
        }
    }
}
#endif

// ---------------------------------------------------------------------------
// Weak default stub for port functions added in this release.
// Ports that don't yet implement them will link against these.
// ---------------------------------------------------------------------------

__attribute__((weak)) void     port_cpu_idle(void)                 { }
__attribute__((weak)) uint32_t port_suppress_ticks(uint32_t n)     { (void)n; return 0; }
__attribute__((weak)) uint8_t  port_core_id(void)                  { return 0; }

// ---------------------------------------------------------------------------
// Weak overflow hook — spin by default; user may override to log / halt.
// ---------------------------------------------------------------------------

__attribute__((weak))
void rtos_stack_overflow_hook(rtos_tcb_t *tcb)
{
    (void)tcb;
    for (;;) ;
}

// ---------------------------------------------------------------------------
// Trace weak stubs — do nothing; user overrides the ones they want.
// ---------------------------------------------------------------------------

#if RTOS_ENABLE_TRACE
__attribute__((weak)) void rtos_trace_task_switched_in(rtos_tcb_t *t)  { (void)t; }
__attribute__((weak)) void rtos_trace_task_switched_out(rtos_tcb_t *t) { (void)t; }
__attribute__((weak)) void rtos_trace_task_create(rtos_tcb_t *t)       { (void)t; }
__attribute__((weak)) void rtos_trace_task_delete(rtos_tcb_t *t)       { (void)t; }
#endif

//[]---------------------------------------------------------------------------[]
// Internal helpers
//[]---------------------------------------------------------------------------[]

//---------------------------------------------------------------------------
// Add a task to the ready list and set its state to TASK_READY. Caller must 
// be in critical section.
//---------------------------------------------------------------------------
static void ready_add(rtos_tcb_t *tcb)
{
    tcb->state = TASK_READY;
#if RTOS_NUM_CORES > 1
    uint8_t c = tcb->core;
    list_insert_tail(&g_ready[c][tcb->priority], tcb);
    g_ready_bitmap[c] |= (1u << tcb->priority);
#else
    list_insert_tail(&g_ready[tcb->priority], tcb);
    g_ready_bitmap |= (1u << tcb->priority);
#endif
}

//---------------------------------------------------------------------------
// Remove a task from the per-core blocked list if it is on it.
// Caller must be in a critical section (or the only thread accessing g_blocked).
//---------------------------------------------------------------------------
void rtos_task_blocked_remove(rtos_tcb_t *tcb)
{
    if (!tcb->on_blocked) return;
    tcb->on_blocked = 0;
#if RTOS_NUM_CORES > 1
    list_remove(&g_blocked[tcb->core], tcb);
#else
    list_remove(&g_blocked, tcb);
#endif
}

//---------------------------------------------------------------------------
// Add a task to the per-core blocked list sorted by absolute wakeup_tick.
// Called from IPC primitives when blocking with a finite timeout.
// Caller must be in a critical section.
//---------------------------------------------------------------------------
void rtos_task_blocked_add(rtos_tcb_t *tcb, uint32_t timeout_ticks)
{
    if (timeout_ticks == RTOS_WAIT_FOREVER) return;
    tcb->wakeup_tick = G_TICK_COUNT + timeout_ticks;
    tcb->on_blocked  = 1;
    list_insert_sorted(&G_BLOCKED, tcb, tcb->wakeup_tick);
}

//---------------------------------------------------------------------------
// Exposed for semaphore/mutex/queue to unblock a task. Also removes the task
// from the per-core blocked list if it is waiting there with a timeout.
//---------------------------------------------------------------------------
void rtos_task_make_ready(rtos_tcb_t *tcb)
{
    rtos_task_blocked_remove(tcb);
    ready_add(tcb);
}

//---------------------------------------------------------------------------
// Remove a task from the ready list. Caller must be in critical section.
//---------------------------------------------------------------------------
static void ready_remove(rtos_tcb_t *tcb)
{
#if RTOS_NUM_CORES > 1
    uint8_t c = tcb->core;
    list_remove(&g_ready[c][tcb->priority], tcb);
    if (!g_ready[c][tcb->priority])
        g_ready_bitmap[c] &= ~(1u << tcb->priority);
#else
    list_remove(&g_ready[tcb->priority], tcb);
    if (!g_ready[tcb->priority])
        g_ready_bitmap &= ~(1u << tcb->priority);
#endif
}

//---------------------------------------------------------------------------
// Return the TCB that should run next (highest priority = lowest index with
// bit set).
//---------------------------------------------------------------------------
static rtos_tcb_t *scheduler_pick_next(void)
{
#if RTOS_NUM_CORES > 1
    if (!G_READY_BITMAP) return &g_idle_tcb[CORE];
#else
    if (!G_READY_BITMAP) return &g_idle_tcb;
#endif

    // __builtin_ctz gives index of lowest set bit = highest priority
    uint8_t prio = (uint8_t)__builtin_ctz(G_READY_BITMAP);

    return G_READY[prio];
}

//[]---------------------------------------------------------------------------[]
// Public API
//[]---------------------------------------------------------------------------[]

//---------------------------------------------------------------------------
// Internal: create a task pinned to a specified core. Both public APIs call
// this so the stack init, watermark, and ready_add logic live in one place.
//---------------------------------------------------------------------------
static rtos_handle_t task_create_impl(rtos_tcb_t *tcb,
                                      void       *stack,
                                      size_t      stack_words,
                                      void      (*func)(void *),
                                      void       *arg,
                                      const char *name,
                                      uint8_t     priority,
                                      uint8_t     core)
{
    if (!tcb || !stack || !func || priority >= RTOS_MAX_PRIORITIES)
        return NULL;

    // Initialise TCB fields
    size_t len = 0;

    while (name && name[len] && len < RTOS_TASK_NAME_LEN - 1) {
        tcb->name[len] = name[len];
        len++;
    }
    
    tcb->name[len]   = '\0';
    tcb->priority    = priority;
    tcb->base_priority = priority;
    tcb->state       = TASK_READY;
    tcb->wakeup_tick = 0;
    tcb->on_blocked  = 0;
    tcb->stack_base  = stack;
    tcb->stack_words = stack_words;
    tcb->next        = NULL;

#if RTOS_NUM_CORES > 1
    tcb->core        = core;
#else
    (void)core;
#endif

#if RTOS_ENABLE_RUNTIME_STATS
    tcb->runtime_ticks = 0;
#endif

    // Fill stack with watermark pattern before writing the sentinel, so the
    // sentinel at the bottom is distinct from the watermark region above it.
#if RTOS_STACK_WATERMARK
    {
        uint32_t *p   = (uint32_t *)stack;
        size_t    n   = stack_words * RTOS_STACK_BYTES_PER_WORD / sizeof(uint32_t);
        for (size_t i = 0; i < n; i++)
            p[i] = STACK_WATERMARK_WORD;
    }
#endif

    // Write sentinel words at the very bottom of the stack (lowest addresses).
#if RTOS_STACK_OVERFLOW_CHECK
    {
        uint32_t *p = (uint32_t *)stack;
        for (int i = 0; i < STACK_SENTINEL_COUNT; i++)
            p[i] = STACK_SENTINEL_WORD;
    }
#endif

    // Build initial stack frame (port-specific).
    // stack_top = one byte past the end of the stack buffer.
    tcb->sp = port_init_stack((uint8_t *)stack + stack_words * RTOS_STACK_BYTES_PER_WORD,
                               func, arg);

#if RTOS_ENABLE_RUNTIME_STATS || RTOS_STACK_OVERFLOW_CHECK
    all_tasks_add(tcb);
#endif

    port_enter_critical();
    ready_add(tcb);
    port_exit_critical();

    RTOS_TRACE_TASK_CREATE(tcb);

    return (rtos_handle_t)tcb;
}

//---------------------------------------------------------------------------
// Create a new task on the calling core. Returns a handle to the task, or
// NULL on failure. Caller must provide a TCB and stack buffer, which can be
// on the caller's stack or in static memory. The task will be added to the
// ready list and may run immediately if it has higher priority than the
// current task.
//---------------------------------------------------------------------------
rtos_handle_t rtos_task_create(rtos_tcb_t *tcb,
                               void       *stack,
                               size_t      stack_words,
                               void      (*func)(void *),
                               void       *arg,
                               const char *name,
                               uint8_t     priority)
{
#if RTOS_NUM_CORES > 1
    return task_create_impl(tcb, stack, stack_words, func, arg, name, priority, CORE);
#else
    return task_create_impl(tcb, stack, stack_words, func, arg, name, priority, 0);
#endif
}

//---------------------------------------------------------------------------
// Create a task pinned to a specific CPU core. Only available when
// RTOS_NUM_CORES > 1.
//---------------------------------------------------------------------------
#if RTOS_NUM_CORES > 1
rtos_handle_t rtos_task_create_on_core(rtos_tcb_t *tcb,
                                       void       *stack,
                                       size_t      stack_words,
                                       void      (*func)(void *),
                                       void       *arg,
                                       const char *name,
                                       uint8_t     priority,
                                       uint8_t     core)
{
    return task_create_impl(tcb, stack, stack_words, func, arg, name, priority, core);
}
#endif

//---------------------------------------------------------------------------
// Delay the current task for a number of ticks. If ticks is 0, yield instead.
//---------------------------------------------------------------------------
void rtos_task_delay(uint32_t ticks)
{
    if (ticks == 0) {
        rtos_task_yield();
        return;
    }

    port_enter_critical();
    ready_remove(G_CURRENT);
    G_CURRENT->state       = TASK_BLOCKED;
    G_CURRENT->wakeup_tick = G_TICK_COUNT + ticks;
    G_CURRENT->on_blocked  = 1;
    list_insert_sorted(&G_BLOCKED, G_CURRENT, G_CURRENT->wakeup_tick);
    port_exit_critical();

    port_request_reschedule();
}

//---------------------------------------------------------------------------
// Yield the CPU to another ready task of the same priority, if any. Otherwise
// does nothing.
//---------------------------------------------------------------------------
void rtos_task_yield(void)
{
    port_request_reschedule();
}

//---------------------------------------------------------------------------
// Suspend a task, preventing it from running until resumed. If task is NULL,
// suspend the current task. If the task is already suspended, does nothing.
//---------------------------------------------------------------------------
void rtos_task_suspend(rtos_handle_t task)
{
    rtos_tcb_t *tcb = task ? (rtos_tcb_t *)task : G_CURRENT;

    port_enter_critical();

    if (tcb->state == TASK_READY || tcb->state == TASK_RUNNING) 
    {
        ready_remove(tcb);
    } else if (tcb->state == TASK_BLOCKED) 
    {
        rtos_task_blocked_remove(tcb);
    }

    tcb->state = TASK_SUSPENDED;
    port_exit_critical();

    if (tcb == G_CURRENT)
        port_request_reschedule();
}

//---------------------------------------------------------------------------
// Resume a suspended task, making it ready to run. If the task is not 
// suspended, does nothing.
//---------------------------------------------------------------------------
void rtos_task_resume(rtos_handle_t task)
{
    rtos_tcb_t *tcb = (rtos_tcb_t *)task;

    if (!tcb || tcb->state != TASK_SUSPENDED) return;

    port_enter_critical();
    ready_add(tcb);
    port_exit_critical();

    port_request_reschedule();
}

//---------------------------------------------------------------------------
// Delete a task, removing it from all lists and marking its state as TASK_DELETED.
// If task is NULL, delete the current task. The task's TCB and stack are not
// freed, so the caller can reuse them to create a new task later if desired.
// If the current task is deleted, the scheduler will immediately switch to another
// ready task.
//---------------------------------------------------------------------------
void rtos_task_delete(rtos_handle_t task)
{
    rtos_tcb_t *tcb = task ? (rtos_tcb_t *)task : G_CURRENT;

    port_enter_critical();

    if (tcb->state == TASK_READY || tcb->state == TASK_RUNNING)
    {
        ready_remove(tcb);
    } else if (tcb->state == TASK_BLOCKED) 
    {
        rtos_task_blocked_remove(tcb);
    }

    tcb->state = TASK_DELETED;
    port_exit_critical();

    RTOS_TRACE_TASK_DELETE(tcb);

#if RTOS_ENABLE_RUNTIME_STATS || RTOS_STACK_OVERFLOW_CHECK
    all_tasks_remove(tcb);
#endif

    if (tcb == G_CURRENT)
        port_request_reschedule();
}

//---------------------------------------------------------------------------
// Get the current tick count, which increments at a constant rate defined by
// RTOS_TICK_RATE_HZ. Used for timing and delays.
//---------------------------------------------------------------------------
uint32_t rtos_task_tick_count(void)
{
    return G_TICK_COUNT;
}

//---------------------------------------------------------------------------
// Delay until an absolute tick deadline, eliminating period drift.
// *last_wake_tick is updated to the next deadline on each call.
//---------------------------------------------------------------------------
void rtos_task_delay_until(uint32_t *last_wake_tick, uint32_t period_ticks)
{
    uint32_t next = *last_wake_tick + period_ticks;
    *last_wake_tick = next;

    uint32_t now = G_TICK_COUNT;
    int32_t remaining = (int32_t)(next - now);
    if (remaining > 0)
        rtos_task_delay((uint32_t)remaining);
}

//---------------------------------------------------------------------------
// Send a notification to a task. If the task is blocked waiting for a
// notification, unblock it immediately.
//---------------------------------------------------------------------------
void rtos_task_notify(rtos_handle_t task)
{
    rtos_tcb_t *tcb = (rtos_tcb_t *)task;
    if (!tcb) return;

    port_enter_critical();
    tcb->notif_pending = 1;

    if (tcb->state == TASK_BLOCKED) {
        rtos_task_make_ready(tcb);
    }

    port_exit_critical();
    port_request_reschedule();
}

//---------------------------------------------------------------------------
// Send a notification from an ISR. Does not trigger a reschedule — the
// caller must call port_request_reschedule() if needed.
//---------------------------------------------------------------------------
void rtos_task_notify_from_isr(rtos_handle_t task)
{
    rtos_tcb_t *tcb = (rtos_tcb_t *)task;
    if (!tcb) return;

    tcb->notif_pending = 1;

    if (tcb->state == TASK_BLOCKED) {
        rtos_task_make_ready(tcb);
    }
}

//---------------------------------------------------------------------------
// Wait for a notification. Clears the pending flag and returns RTOS_OK when
// notified. Returns RTOS_TIMEOUT if the timeout expires.
//---------------------------------------------------------------------------
int rtos_task_notify_wait(uint32_t timeout_ticks)
{
    port_enter_critical();

    if (G_CURRENT->notif_pending) {
        G_CURRENT->notif_pending = 0;
        port_exit_critical();
        return RTOS_OK;
    }

    if (timeout_ticks == RTOS_NO_WAIT) {
        port_exit_critical();
        return RTOS_TIMEOUT;
    }

    G_CURRENT->state       = TASK_BLOCKED;
    G_CURRENT->wakeup_tick = G_TICK_COUNT + timeout_ticks;
    G_CURRENT->on_blocked  = 1;
    ready_remove(G_CURRENT);
    list_insert_sorted(&G_BLOCKED, G_CURRENT, G_CURRENT->wakeup_tick);
    port_exit_critical();

    port_request_reschedule();

    port_enter_critical();
    int notified = G_CURRENT->notif_pending;
    G_CURRENT->notif_pending = 0;
    port_exit_critical();

    return notified ? RTOS_OK : RTOS_TIMEOUT;
}

//---------------------------------------------------------------------------
// Debug: check whether a task's stack sentinel is still intact.
//---------------------------------------------------------------------------
int rtos_task_check_stack(rtos_handle_t task)
{
#if RTOS_STACK_OVERFLOW_CHECK
    rtos_tcb_t *tcb = (rtos_tcb_t *)task;
    if (!tcb) return RTOS_ERR;
    uint32_t *p = (uint32_t *)tcb->stack_base;
    return (p[0] == STACK_SENTINEL_WORD) ? RTOS_OK : RTOS_ERR;
#else
    (void)task;
    return RTOS_OK;
#endif
}

//---------------------------------------------------------------------------
// Debug: return the number of stack words never written (high-water mark).
//---------------------------------------------------------------------------
uint32_t rtos_task_stack_high_water_mark(rtos_handle_t task)
{
#if RTOS_STACK_WATERMARK
    rtos_tcb_t *tcb = (rtos_tcb_t *)task;
    if (!tcb) return 0;
    uint32_t *p      = (uint32_t *)tcb->stack_base;
    size_t    words  = tcb->stack_words * RTOS_STACK_BYTES_PER_WORD / sizeof(uint32_t);
    uint32_t  untouched = 0;
    // Walk from bottom up; skip sentinel region
    for (size_t i = STACK_SENTINEL_COUNT; i < words; i++) {
        if (p[i] == STACK_WATERMARK_WORD)
            untouched++;
        else
            break;
    }
    return untouched;
#else
    (void)task;
    return 0;
#endif
}

//---------------------------------------------------------------------------
// Runtime CPU stats
//---------------------------------------------------------------------------
#if RTOS_ENABLE_RUNTIME_STATS
size_t rtos_task_get_runtime_stats(rtos_runtime_stat_t *buf, size_t n)
{
    if (!buf || n == 0) return 0;

    // Compute total ticks across all live tasks
    uint32_t total = 0;
    for (size_t i = 0; i < g_all_tasks_count; i++)
        total += g_all_tasks[i]->runtime_ticks;

    size_t written = 0;
    for (size_t i = 0; i < g_all_tasks_count && written < n; i++, written++) {
        buf[written].name          = g_all_tasks[i]->name;
        buf[written].runtime_ticks = g_all_tasks[i]->runtime_ticks;
        buf[written].percent       = total ? (uint8_t)((uint64_t)g_all_tasks[i]->runtime_ticks * 100 / total) : 0;
    }
    return written;
}
#endif

//---------------------------------------------------------------------------
// Idle task implementation
//---------------------------------------------------------------------------
#if RTOS_STACK_OVERFLOW_CHECK
static void idle_check_all_stacks(void)
{
    for (size_t i = 0; i < g_all_tasks_count; i++) {
        uint32_t *p = (uint32_t *)g_all_tasks[i]->stack_base;
        if (p[0] != STACK_SENTINEL_WORD)
            rtos_stack_overflow_hook(g_all_tasks[i]);
    }
}
#endif

static void idle_task(void *arg)
{
    (void)arg;
    for (;;) {
#if RTOS_STACK_OVERFLOW_CHECK
        idle_check_all_stacks();
#endif

#ifdef RTOS_IDLE_HOOK_FUNCTION
        RTOS_IDLE_HOOK_FUNCTION();
#endif

#if RTOS_TICKLESS_IDLE
        {
            uint32_t max = rtos_idle_next_wakeup_ticks();
            if (max > 1) {
                uint32_t elapsed = port_suppress_ticks(max);
                if (elapsed > 0)
                    rtos_tick_advance(elapsed);
            }
        }
#endif
        port_cpu_idle();
        port_request_reschedule();
    }
}

//---------------------------------------------------------------------------
// Tickless: walk blocked list, return ticks until the soonest wakeup.
//---------------------------------------------------------------------------
uint32_t rtos_idle_next_wakeup_ticks(void)
{
    if (!G_BLOCKED) return RTOS_WAIT_FOREVER;
    uint32_t now = G_TICK_COUNT;
    return G_BLOCKED->wakeup_tick > now ? G_BLOCKED->wakeup_tick - now : 0;
}

//---------------------------------------------------------------------------
// Bulk-advance the tick count (used by tickless idle after port_suppress_ticks).
//---------------------------------------------------------------------------
void rtos_tick_advance(uint32_t n)
{
    G_TICK_COUNT += n;

    // Unblock all tasks that reached their absolute wakeup tick (O(k))
    while (G_BLOCKED && G_BLOCKED->wakeup_tick <= G_TICK_COUNT) {
        rtos_tcb_t *expired = list_pop_head(&G_BLOCKED);
        expired->on_blocked = 0;
        ready_add(expired);
    }

    // Timers use countdowns, so tick them N times to maintain accuracy
    extern void rtos_timer_tick(void);
    for (uint32_t i = 0; i < n; i++)
        rtos_timer_tick();
}

//---------------------------------------------------------------------------
// Tick handler — called from the port's SysTick ISR
//---------------------------------------------------------------------------
void rtos_tick_handler(void)
{
    G_TICK_COUNT++;

#if RTOS_ENABLE_RUNTIME_STATS
    if (G_CURRENT)
        G_CURRENT->runtime_ticks++;
#endif

    // Unblock tasks whose absolute wakeup tick has arrived.
    // Blocked list is sorted ascending by wakeup_tick — only check the head (O(k)).
    while (G_BLOCKED && G_BLOCKED->wakeup_tick <= G_TICK_COUNT)
    {
        rtos_tcb_t *expired = list_pop_head(&G_BLOCKED);
        expired->on_blocked = 0;
        ready_add(expired);
    }

    // Per-tick stack overflow check (lightweight: only tests first sentinel word)
#if RTOS_STACK_OVERFLOW_CHECK
    if (G_CURRENT) {
        uint32_t *p = (uint32_t *)G_CURRENT->stack_base;
        if (p && p[0] != STACK_SENTINEL_WORD)
            rtos_stack_overflow_hook(G_CURRENT);
    }
#endif

    // Fire software timers
    extern void rtos_timer_tick(void);
    rtos_timer_tick();

    port_request_reschedule();
}

// ---------------------------------------------------------------------------
// Scheduler start — called by vRTOSStart() after port_init()
// ---------------------------------------------------------------------------

// Exposed so the port's context switcher can read/write the current TCB pointer.
#if RTOS_NUM_CORES > 1
rtos_tcb_t **rtos_current_tcb_ptr(void) { return &g_current[port_core_id()]; }
rtos_tcb_t  *rtos_next_task(void)       { return scheduler_pick_next(); }
#else
rtos_tcb_t **rtos_current_tcb_ptr(void) { return &g_current; }
rtos_tcb_t  *rtos_next_task(void)       { return scheduler_pick_next(); }
#endif

// Perform a context switch: if the current task was preempted (still RUNNING),
// put it back on the ready list, then pick the highest-priority ready task and
// make it the new current task.  Called from PendSV (ARM) or the timer ISR (AVR).
void rtos_context_switch(void)
{
    if (G_CURRENT && G_CURRENT->state == TASK_RUNNING) {
        RTOS_TRACE_TASK_SWITCHED_OUT(G_CURRENT);
        ready_add(G_CURRENT);
    }

    rtos_tcb_t *next = scheduler_pick_next();
    G_CURRENT = next;
    G_CURRENT->state = TASK_RUNNING;
    ready_remove(G_CURRENT);

    RTOS_TRACE_TASK_SWITCHED_IN(G_CURRENT);
}

//---------------------------------------------------------------------------
// Initialise the idle task and start the scheduler. Called by rtos_start().
//---------------------------------------------------------------------------
static void rtos_scheduler_start(void)
{
#if RTOS_NUM_CORES > 1
    uint8_t core = port_core_id();
    rtos_task_create(&g_idle_tcb[core], g_idle_stack[core], RTOS_IDLE_STACK_WORDS,
                     idle_task, NULL, "idle", RTOS_MAX_PRIORITIES - 1);
    G_CURRENT = scheduler_pick_next();
#else
    rtos_task_create(&g_idle_tcb, g_idle_stack, RTOS_IDLE_STACK_WORDS, idle_task, NULL,
                "idle", RTOS_MAX_PRIORITIES - 1);
    g_current = scheduler_pick_next();
#endif

    G_CURRENT->state = TASK_RUNNING;
    ready_remove(G_CURRENT);
}

//---------------------------------------------------------------------------
// rtos_start — initialise the port and start the scheduler. Never returns.
//---------------------------------------------------------------------------
void rtos_start(void)
{
    extern void port_init(uint32_t tick_rate_hz);
    extern void port_start_first_task(void);

    port_init(RTOS_TICK_RATE_HZ);

    rtos_scheduler_start();
    
    port_start_first_task();

    // Never reached
    for (;;) ;
}

//---------------------------------------------------------------------------
// rtos_core1_entry — entry point for core 1. Pass to multicore_launch_core1()
// before calling rtos_start() on core 0. Configures core 1's SysTick and
// interrupt priorities, creates core 1's idle task, then starts scheduling.
// Never returns.
//---------------------------------------------------------------------------
#if RTOS_NUM_CORES > 1
void rtos_core1_entry(void)
{
    extern void port_init(uint32_t tick_rate_hz);
    extern void port_start_first_task(void);

    // SysTick and the ARM SCB priority registers are banked per-core on RP2040,
    // so calling port_init() here configures this core independently.
    port_init(RTOS_TICK_RATE_HZ);

    rtos_scheduler_start();

    port_start_first_task();

    // Never reached
    for (;;) ;
}
#endif


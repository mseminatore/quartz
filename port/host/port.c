// Copyright 2025. All rights reserved.
//
// Host simulation port — macOS / Linux.
//
// Uses POSIX ucontext for cooperative context switching so that all RTOS
// primitives (tasks, semaphores, queues, mutexes, timers) work correctly on
// the development machine without any hardware.
//
// Tick delivery: one tick is delivered each time a task calls
// port_request_reschedule (i.e., on every delay / yield / blocking call).
// This makes rtos_task_delay(n) step n ticks cooperatively rather than in
// real wall-clock time.  A usleep(1000) is inserted per tick so the
// simulation does not spin at full CPU speed.

// Suppress deprecation warnings for ucontext (still functional on macOS)
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#define _XOPEN_SOURCE 700
#include <ucontext.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "../../include/rtos_task.h"
#include "../../src/port.h"
#include "../../include/rtos_config.h"

// ---------------------------------------------------------------------------
// Kernel symbols we need from task.c
// ---------------------------------------------------------------------------

extern rtos_tcb_t  *g_current;
extern rtos_tcb_t **rtos_current_tcb_ptr(void);
extern void         rtos_context_switch(void);
extern void         rtos_tick_handler(void);

// ---------------------------------------------------------------------------
// Per-task context storage
// Each task's ucontext_t pointer is stored in tcb->sp.
// The actual stacks come from a static pool so we don't touch the
// user-provided stack buffer (its size is unknown to port_init_stack).
// ---------------------------------------------------------------------------

#define HOST_MAX_TASKS   (RTOS_MAX_TASKS + 1)   // +1 for idle
#define HOST_STACK_SIZE  65536                   // 64 KB per simulated task

static ucontext_t g_ctx[HOST_MAX_TASKS];
static uint8_t    g_stacks[HOST_MAX_TASKS][HOST_STACK_SIZE];
static int        g_ctx_count = 0;

// Trampoline arguments — makecontext can only pass ints so we use a table
static void (*g_funcs[HOST_MAX_TASKS])(void *);
static void  *g_args[HOST_MAX_TASKS];

// Scheduler context — swapped to when a task yields
static ucontext_t g_sched_ctx;

// Guard against recursive tick delivery
static int g_delivering_tick = 0;

// ---------------------------------------------------------------------------
// Critical section — single-threaded simulation, no-op
// ---------------------------------------------------------------------------

void port_enter_critical(void) { }
void port_exit_critical(void)  { }

// ---------------------------------------------------------------------------
// Trampoline: calls the actual task function after context is activated
// ---------------------------------------------------------------------------

static void task_trampoline(void)
{
    // Identify which context slot we occupy by matching tcb->sp
    rtos_tcb_t  *self = *rtos_current_tcb_ptr();
    ucontext_t  *me   = (ucontext_t *)self->sp;
    int idx = (int)(me - g_ctx);
    g_funcs[idx](g_args[idx]);

    // Task returned (should not happen in a real RTOS application)
    for (;;)
        port_request_reschedule();
}

// ---------------------------------------------------------------------------
// Stack / context initialisation (called by rtos_task_create for each task)
// ---------------------------------------------------------------------------

void *port_init_stack(void     *stack_top,
                      void    (*func)(void *),
                      void     *arg)
{
    int idx = g_ctx_count++;
    if (idx >= HOST_MAX_TASKS) {
        fprintf(stderr, "port/host: too many tasks (increase HOST_MAX_TASKS)\n");
        return NULL;
    }

    g_funcs[idx] = func;
    g_args[idx]  = arg;

    getcontext(&g_ctx[idx]);
    g_ctx[idx].uc_stack.ss_sp   = g_stacks[idx];
    g_ctx[idx].uc_stack.ss_size = HOST_STACK_SIZE;
    g_ctx[idx].uc_link          = NULL;
    makecontext(&g_ctx[idx], task_trampoline, 0);

    (void)stack_top;   // host uses its own stack pool
    return &g_ctx[idx];
}

// ---------------------------------------------------------------------------
// Request a context switch (called from rtos_task_delay, yield, blocking ops)
// ---------------------------------------------------------------------------

void port_request_reschedule(void)
{
    if (g_delivering_tick) return;   // prevent recursion from rtos_tick_handler

    rtos_tcb_t *self = *rtos_current_tcb_ptr();
    if (!self) return;

    ucontext_t *my_ctx = (ucontext_t *)self->sp;

    // Advance the simulated clock by one tick before switching
    g_delivering_tick = 1;
    usleep(1000);                    // ~1 ms real time per tick
    rtos_tick_handler();             // count++, unblock delayed tasks, fire timers
    g_delivering_tick = 0;

    // Select the next task and yield to the scheduler
    rtos_context_switch();
    swapcontext(my_ctx, &g_sched_ctx);
    // Execution resumes here when this task is switched back in
}

// ---------------------------------------------------------------------------
// Port initialisation (tick rate is simulated via usleep, not hardware)
// ---------------------------------------------------------------------------

void port_init(uint32_t tick_rate_hz) { (void)tick_rate_hz; }

// ---------------------------------------------------------------------------
// Hand execution to the first task; never returns
// ---------------------------------------------------------------------------

void port_start_first_task(void)
{
    // Scheduler loop: resume whatever g_current points at.
    // g_current is already set by rtos_scheduler_start() before we arrive here.
    for (;;) {
        ucontext_t *next_ctx = (ucontext_t *)g_current->sp;
        swapcontext(&g_sched_ctx, next_ctx);
        // A task just yielded back to us; g_current is already updated by
        // port_request_reschedule before the swapcontext there.
    }
}

// ---------------------------------------------------------------------------
// port_cpu_idle — no-op on the host; tick advancement happens inside
// port_request_reschedule (which is called immediately after in the idle loop).
// ---------------------------------------------------------------------------

void port_cpu_idle(void) { }

// ---------------------------------------------------------------------------
// port_core_id — single-threaded simulation, always core 0.
// ---------------------------------------------------------------------------

uint8_t port_core_id(void) { return 0; }

// ---------------------------------------------------------------------------
// port_suppress_ticks — simulated tickless idle for the host port.
// Sleeps max_ticks milliseconds (one tick = 1 ms on the host) and returns the
// number of ticks slept.  rtos_tick_advance() in task.c credits them to the
// scheduler.  The idle task's port_request_reschedule() call that normally
// follows will NOT deliver another tick (g_delivering_tick guard), so there
// is no double-counting.
// ---------------------------------------------------------------------------

#if RTOS_TICKLESS_IDLE
uint32_t port_suppress_ticks(uint32_t max_ticks)
{
    if (max_ticks == 0) return 0;
    usleep((useconds_t)max_ticks * 1000u);  // 1 tick = 1 ms on the host
    return max_ticks;
}
#endif  // RTOS_TICKLESS_IDLE

#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif

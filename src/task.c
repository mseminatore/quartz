//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Task management and scheduler.
//---------------------------------------------------------------------------
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../include/rtos_task.h"
#include "list.h"
#include "port.h"

//[]---------------------------------------------------------------------------[]
// Scheduler state
//[]---------------------------------------------------------------------------[]

static rtos_tcb_t  *g_ready[RTOS_MAX_PRIORITIES];   // per-priority ready queues
static uint32_t     g_ready_bitmap;                 // bit N set ↔ g_ready[N] non-empty
static rtos_tcb_t  *g_blocked;                      // delay-sorted blocked list
rtos_tcb_t         *g_current;                      // currently running task (non-static for port asm)
static uint32_t     g_tick_count;                   // incremented by SysTick handler, returned by xTaskGetTickCount()

// Idle task — runs when no other task is ready.
static rtos_tcb_t   g_idle_tcb;
static uint8_t      g_idle_stack[RTOS_IDLE_STACK_WORDS * RTOS_STACK_BYTES_PER_WORD];

//---------------------------------------------------------------------------
// Idle task implementation
//---------------------------------------------------------------------------
static void idle_task(void *arg)
{
    (void)arg;
    for (;;)
        ; // could WFI here on real hardware
}

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
    list_insert_tail(&g_ready[tcb->priority], tcb);
    g_ready_bitmap |= (1u << tcb->priority);
}

//---------------------------------------------------------------------------
// Exposed for semaphore/mutex/queue to unblock a task.
//---------------------------------------------------------------------------
void rtos_task_make_ready(rtos_tcb_t *tcb)
{
    ready_add(tcb);
}

//---------------------------------------------------------------------------
// Remove a task from the ready list. Caller must be in critical section.
//---------------------------------------------------------------------------
static void ready_remove(rtos_tcb_t *tcb)
{
    list_remove(&g_ready[tcb->priority], tcb);

    if (!g_ready[tcb->priority])
        g_ready_bitmap &= ~(1u << tcb->priority);
}

//---------------------------------------------------------------------------
// Return the TCB that should run next (highest priority = lowest index with
// bit set).
//---------------------------------------------------------------------------
static rtos_tcb_t *scheduler_pick_next(void)
{
    if (!g_ready_bitmap) return &g_idle_tcb;

    // __builtin_ctz gives index of lowest set bit = highest priority
    uint8_t prio = (uint8_t)__builtin_ctz(g_ready_bitmap);

    return g_ready[prio];
}

//[]---------------------------------------------------------------------------[]
// Public API
//[]---------------------------------------------------------------------------[]

//---------------------------------------------------------------------------
// Create a new task. Returns a handle to the task, or NULL on failure.
// Caller must provide a TCB and stack buffer, which can be on the caller's 
// stack or in static memory. The task will be added to the ready list and 
// may run immediately if it has higher priority than the current task.
//---------------------------------------------------------------------------
rtos_handle_t xTaskCreate(rtos_tcb_t *tcb,
                           void       *stack,
                           size_t      stack_words,
                           void      (*func)(void *),
                           void       *arg,
                           const char *name,
                           uint8_t     priority)
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
    tcb->state       = TASK_READY;
    tcb->delay_ticks = 0;
    tcb->stack_base  = stack;
    tcb->stack_words = stack_words;
    tcb->next        = NULL;

    // Build initial stack frame (port-specific).
    // stack_top = one byte past the end of the stack buffer.
    tcb->sp = port_init_stack((uint8_t *)stack + stack_words * RTOS_STACK_BYTES_PER_WORD,
                               func, arg);

    port_enter_critical();
    ready_add(tcb);
    port_exit_critical();

    return (rtos_handle_t)tcb;
}

//---------------------------------------------------------------------------
// Delay the current task for a number of ticks. If ticks is 0, yield instead.
//---------------------------------------------------------------------------
void vTaskDelay(uint32_t ticks)
{
    if (ticks == 0) {
        vTaskYield();
        return;
    }

    port_enter_critical();
    ready_remove(g_current);
    g_current->state       = TASK_BLOCKED;
    g_current->delay_ticks = ticks;
    list_insert_sorted(&g_blocked, g_current, ticks);
    port_exit_critical();

    port_request_reschedule();
}

//---------------------------------------------------------------------------
// Yield the CPU to another ready task of the same priority, if any. Otherwise
// does nothing.
//---------------------------------------------------------------------------
void vTaskYield(void)
{
    port_request_reschedule();
}

//---------------------------------------------------------------------------
// Suspend a task, preventing it from running until resumed. If task is NULL,
// suspend the current task. If the task is already suspended, does nothing.
//---------------------------------------------------------------------------
void vTaskSuspend(rtos_handle_t task)
{
    rtos_tcb_t *tcb = task ? (rtos_tcb_t *)task : g_current;

    port_enter_critical();

    if (tcb->state == TASK_READY || tcb->state == TASK_RUNNING) 
    {
        ready_remove(tcb);
    } else if (tcb->state == TASK_BLOCKED) 
    {
        list_remove(&g_blocked, tcb);
    }

    tcb->state = TASK_SUSPENDED;
    port_exit_critical();

    if (tcb == g_current)
        port_request_reschedule();
}

//---------------------------------------------------------------------------
// Resume a suspended task, making it ready to run. If the task is not 
// suspended, does nothing.
//---------------------------------------------------------------------------
void vTaskResume(rtos_handle_t task)
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
void vTaskDelete(rtos_handle_t task)
{
    rtos_tcb_t *tcb = task ? (rtos_tcb_t *)task : g_current;

    port_enter_critical();

    if (tcb->state == TASK_READY || tcb->state == TASK_RUNNING)
    {
        ready_remove(tcb);
    } else if (tcb->state == TASK_BLOCKED) 
    {
        list_remove(&g_blocked, tcb);
    }

    tcb->state = TASK_DELETED;
    port_exit_critical();

    if (tcb == g_current)
        port_request_reschedule();
}

//---------------------------------------------------------------------------
// Get the current tick count, which increments at a constant rate defined by
// RTOS_TICK_RATE_HZ. Used for timing and delays.
//---------------------------------------------------------------------------
uint32_t xTaskGetTickCount(void)
{
    return g_tick_count;
}

//---------------------------------------------------------------------------
// Tick handler — called from the port's SysTick ISR
//---------------------------------------------------------------------------
void rtos_tick_handler(void)
{
    g_tick_count++;

    // Decrement delay counts; unblock tasks whose delay has expired
    rtos_tcb_t *tcb = g_blocked;

    while (tcb) 
    {
        rtos_tcb_t *next = tcb->next;
        if (tcb->delay_ticks > 0)
            tcb->delay_ticks--;

        if (tcb->delay_ticks == 0) 
        {
            list_remove(&g_blocked, tcb);
            ready_add(tcb);
        }

        tcb = next;
    }

    // Fire software timers
    extern void rtos_timer_tick(void);
    rtos_timer_tick();

    port_request_reschedule();
}

// ---------------------------------------------------------------------------
// Scheduler start — called by vRTOSStart() after port_init()
// ---------------------------------------------------------------------------

// Exposed so the port's context switcher can read/write the current TCB pointer.
rtos_tcb_t **rtos_current_tcb_ptr(void) { return &g_current; }
rtos_tcb_t  *rtos_next_task(void)       { return scheduler_pick_next(); }

// Perform a context switch: if the current task was preempted (still RUNNING),
// put it back on the ready list, then pick the highest-priority ready task and
// make it the new current task.  Called from PendSV (ARM) or the timer ISR (AVR).
void rtos_context_switch(void)
{
    if (g_current && g_current->state == TASK_RUNNING)
        ready_add(g_current);

    rtos_tcb_t *next = scheduler_pick_next();
    g_current = next;
    g_current->state = TASK_RUNNING;
    ready_remove(g_current);
}

//---------------------------------------------------------------------------
// Initialise the idle task and start the scheduler. Called by vRTOSStart() after port_init().
//---------------------------------------------------------------------------
static void rtos_scheduler_start(void)
{
    // Create the idle task at the lowest priority
    xTaskCreate(&g_idle_tcb, g_idle_stack, RTOS_IDLE_STACK_WORDS, idle_task, NULL,
                "idle", RTOS_MAX_PRIORITIES - 1);

    // Pick the first task and hand control to the port
    g_current = scheduler_pick_next();
    g_current->state = TASK_RUNNING;

    ready_remove(g_current);
}

//---------------------------------------------------------------------------
// vRTOSStart — initialise the port and start the scheduler. Never returns.
//---------------------------------------------------------------------------
void vRTOSStart(void)
{
    extern void port_init(uint32_t tick_rate_hz);
    extern void port_start_first_task(void);

    port_init(RTOS_TICK_RATE_HZ);

    rtos_scheduler_start();
    
    port_start_first_task();

    // Never reached
    for (;;) ;
}


//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Trace hook macros — instrument key RTOS events at zero runtime cost.
//
// Usage:
//   1. Define RTOS_ENABLE_TRACE 1 before including rtos.h (or in rtos_config.h).
//   2. Implement any rtos_trace_* functions you want to observe (e.g. write to
//      a ring buffer, print to UART, feed Segger SystemView, etc.).
//      Unimplemented hooks resolve to the weak no-op stubs in src/task.c.
//
// When RTOS_ENABLE_TRACE is 0 (the default) every macro expands to ((void)0)
// and the compiler generates no code.
//---------------------------------------------------------------------------
#ifndef RTOS_TRACE_H
#define RTOS_TRACE_H

#include "rtos_task.h"
#include "rtos_config.h"

#if RTOS_ENABLE_TRACE

// ---------------------------------------------------------------------------
// Scheduler events
// ---------------------------------------------------------------------------

// Called just before the context switcher resumes 'tcb'.
void rtos_trace_task_switched_in(rtos_tcb_t *tcb);
#define RTOS_TRACE_TASK_SWITCHED_IN(tcb)  rtos_trace_task_switched_in(tcb)

// Called just after the context switcher suspends 'tcb'.
void rtos_trace_task_switched_out(rtos_tcb_t *tcb);
#define RTOS_TRACE_TASK_SWITCHED_OUT(tcb) rtos_trace_task_switched_out(tcb)

// Called when a task is created.
void rtos_trace_task_create(rtos_tcb_t *tcb);
#define RTOS_TRACE_TASK_CREATE(tcb)       rtos_trace_task_create(tcb)

// Called when a task is deleted.
void rtos_trace_task_delete(rtos_tcb_t *tcb);
#define RTOS_TRACE_TASK_DELETE(tcb)       rtos_trace_task_delete(tcb)

// ---------------------------------------------------------------------------
// Semaphore events
// ---------------------------------------------------------------------------

void rtos_trace_sem_take(void *handle);
#define RTOS_TRACE_SEM_TAKE(h)            rtos_trace_sem_take(h)

void rtos_trace_sem_give(void *handle);
#define RTOS_TRACE_SEM_GIVE(h)            rtos_trace_sem_give(h)

// ---------------------------------------------------------------------------
// Mutex events
// ---------------------------------------------------------------------------

void rtos_trace_mutex_lock(void *handle);
#define RTOS_TRACE_MUTEX_LOCK(h)          rtos_trace_mutex_lock(h)

void rtos_trace_mutex_unlock(void *handle);
#define RTOS_TRACE_MUTEX_UNLOCK(h)        rtos_trace_mutex_unlock(h)

// ---------------------------------------------------------------------------
// Queue events
// ---------------------------------------------------------------------------

void rtos_trace_queue_send(void *handle);
#define RTOS_TRACE_QUEUE_SEND(h)          rtos_trace_queue_send(h)

void rtos_trace_queue_receive(void *handle);
#define RTOS_TRACE_QUEUE_RECEIVE(h)       rtos_trace_queue_receive(h)

// ---------------------------------------------------------------------------
// Timer events
// ---------------------------------------------------------------------------

void rtos_trace_timer_fire(void *handle);
#define RTOS_TRACE_TIMER_FIRE(h)          rtos_trace_timer_fire(h)

#else  // RTOS_ENABLE_TRACE == 0  ----------------------------------------

#define RTOS_TRACE_TASK_SWITCHED_IN(tcb)  ((void)0)
#define RTOS_TRACE_TASK_SWITCHED_OUT(tcb) ((void)0)
#define RTOS_TRACE_TASK_CREATE(tcb)       ((void)0)
#define RTOS_TRACE_TASK_DELETE(tcb)       ((void)0)
#define RTOS_TRACE_SEM_TAKE(h)            ((void)0)
#define RTOS_TRACE_SEM_GIVE(h)            ((void)0)
#define RTOS_TRACE_MUTEX_LOCK(h)          ((void)0)
#define RTOS_TRACE_MUTEX_UNLOCK(h)        ((void)0)
#define RTOS_TRACE_QUEUE_SEND(h)          ((void)0)
#define RTOS_TRACE_QUEUE_RECEIVE(h)       ((void)0)
#define RTOS_TRACE_TIMER_FIRE(h)          ((void)0)

#endif  // RTOS_ENABLE_TRACE

#endif  // RTOS_TRACE_H

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

#ifdef __cplusplus
extern "C" {
#endif

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

// IPC hooks carry two auxiliary u32 fields whose meaning is event-specific:
//   sem_take      aux1=count after take    aux2=1 if blocked path else 0
//   sem_give      aux1=count after give    aux2=1 if woke a waiter else 0
//   mutex_lock    aux1=nest_count          aux2=1 if blocked path else 0
//   mutex_unlock  aux1=nest_count after    aux2=1 if woke a waiter else 0
//   queue_send    aux1=count after send    aux2=1 if woke a receiver else 0
//   queue_recv    aux1=count after recv    aux2=1 if woke a sender else 0
//   timer_fire    aux1=0                   aux2=0

void rtos_trace_sem_take(void *handle, uint32_t aux1, uint32_t aux2);
#define RTOS_TRACE_SEM_TAKE(h, a1, a2)    rtos_trace_sem_take((h), (a1), (a2))

void rtos_trace_sem_give(void *handle, uint32_t aux1, uint32_t aux2);
#define RTOS_TRACE_SEM_GIVE(h, a1, a2)    rtos_trace_sem_give((h), (a1), (a2))

// ---------------------------------------------------------------------------
// Mutex events
// ---------------------------------------------------------------------------

void rtos_trace_mutex_lock(void *handle, uint32_t aux1, uint32_t aux2);
#define RTOS_TRACE_MUTEX_LOCK(h, a1, a2)  rtos_trace_mutex_lock((h), (a1), (a2))

void rtos_trace_mutex_unlock(void *handle, uint32_t aux1, uint32_t aux2);
#define RTOS_TRACE_MUTEX_UNLOCK(h, a1, a2) rtos_trace_mutex_unlock((h), (a1), (a2))

// ---------------------------------------------------------------------------
// Queue events
// ---------------------------------------------------------------------------

void rtos_trace_queue_send(void *handle, uint32_t aux1, uint32_t aux2);
#define RTOS_TRACE_QUEUE_SEND(h, a1, a2)  rtos_trace_queue_send((h), (a1), (a2))

void rtos_trace_queue_receive(void *handle, uint32_t aux1, uint32_t aux2);
#define RTOS_TRACE_QUEUE_RECEIVE(h, a1, a2) rtos_trace_queue_receive((h), (a1), (a2))

// ---------------------------------------------------------------------------
// Timer events
// ---------------------------------------------------------------------------

void rtos_trace_timer_fire(void *handle);
#define RTOS_TRACE_TIMER_FIRE(h)          rtos_trace_timer_fire(h)

// ---------------------------------------------------------------------------
// Event group events
// ---------------------------------------------------------------------------
// eg_set   aux1=bits after set    aux2=number of tasks woken
// eg_wait  aux1=bits that woke    aux2=1 if blocked path else 0

void rtos_trace_eg_set(void *handle, uint32_t aux1, uint32_t aux2);
#define RTOS_TRACE_EG_SET(h, a1, a2)      rtos_trace_eg_set((h), (a1), (a2))

void rtos_trace_eg_wait(void *handle, uint32_t aux1, uint32_t aux2);
#define RTOS_TRACE_EG_WAIT(h, a1, a2)     rtos_trace_eg_wait((h), (a1), (a2))

#else  // RTOS_ENABLE_TRACE == 0  ----------------------------------------

#define RTOS_TRACE_TASK_SWITCHED_IN(tcb)  ((void)0)
#define RTOS_TRACE_TASK_SWITCHED_OUT(tcb) ((void)0)
#define RTOS_TRACE_TASK_CREATE(tcb)       ((void)0)
#define RTOS_TRACE_TASK_DELETE(tcb)       ((void)0)
#define RTOS_TRACE_SEM_TAKE(h, a1, a2)    ((void)0)
#define RTOS_TRACE_SEM_GIVE(h, a1, a2)    ((void)0)
#define RTOS_TRACE_MUTEX_LOCK(h, a1, a2)  ((void)0)
#define RTOS_TRACE_MUTEX_UNLOCK(h, a1, a2) ((void)0)
#define RTOS_TRACE_QUEUE_SEND(h, a1, a2)  ((void)0)
#define RTOS_TRACE_QUEUE_RECEIVE(h, a1, a2) ((void)0)
#define RTOS_TRACE_TIMER_FIRE(h)          ((void)0)
#define RTOS_TRACE_EG_SET(h, a1, a2)      ((void)0)
#define RTOS_TRACE_EG_WAIT(h, a1, a2)     ((void)0)

#endif  // RTOS_ENABLE_TRACE

#ifdef __cplusplus
}
#endif

#endif  // RTOS_TRACE_H

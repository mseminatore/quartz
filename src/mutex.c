// Copyright 2025. All rights reserved.
// Mutex implementation (non-recursive).
#include <stdint.h>
#include "../include/rtos_mutex.h"
#include "list.h"
#include "port.h"

extern rtos_tcb_t  *rtos_next_task(void);
extern rtos_tcb_t **rtos_current_tcb_ptr(void);
extern void         rtos_task_make_ready(rtos_tcb_t *tcb);

#define current_task() (*rtos_current_tcb_ptr())

rtos_handle_t xMutexCreate(rtos_mutex_t *mutex)
{
    if (!mutex) return NULL;
    mutex->owner     = NULL;
    mutex->wait_list = NULL;
    return (rtos_handle_t)mutex;
}

int xMutexLock(rtos_handle_t handle, uint32_t timeout_ticks)
{
    rtos_mutex_t *mutex = (rtos_mutex_t *)handle;
    if (!mutex) return RTOS_ERR;

    port_enter_critical();

    if (!mutex->owner) {
        mutex->owner = current_task();
        port_exit_critical();
        return RTOS_OK;
    }

    if (timeout_ticks == RTOS_NO_WAIT) {
        port_exit_critical();
        return RTOS_TIMEOUT;
    }

    // Block current task
    rtos_tcb_t *self = current_task();
    self->state       = TASK_BLOCKED;
    self->delay_ticks = timeout_ticks;
    list_insert_tail(&mutex->wait_list, self);
    port_exit_critical();

    port_request_reschedule();

    port_enter_critical();
    int on_list = list_remove(&mutex->wait_list, self);
    port_exit_critical();

    return on_list ? RTOS_TIMEOUT : RTOS_OK;
}

void xMutexUnlock(rtos_handle_t handle)
{
    rtos_mutex_t *mutex = (rtos_mutex_t *)handle;
    if (!mutex) return;

    port_enter_critical();

    rtos_tcb_t *waiter = list_pop_head(&mutex->wait_list);
    if (waiter) {
        mutex->owner = waiter;
        rtos_task_make_ready(waiter);
    } else {
        mutex->owner = NULL;
    }

    port_exit_critical();
    port_request_reschedule();
}

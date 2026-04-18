// Copyright 2025. All rights reserved.
// Semaphore implementation (binary and counting).
#include <stdint.h>
#include "../include/rtos_sem.h"
#include "list.h"
#include "port.h"

extern rtos_tcb_t  *rtos_next_task(void);
extern rtos_tcb_t **rtos_current_tcb_ptr(void);
extern void         rtos_task_make_ready(rtos_tcb_t *tcb);

#define current_task() (*rtos_current_tcb_ptr())

rtos_handle_t xSemaphoreCreateBinary(rtos_sem_t *sem)
{
    if (!sem) return NULL;
    sem->count     = 0;
    sem->max_count = 1;
    sem->wait_list = NULL;
    return (rtos_handle_t)sem;
}

rtos_handle_t xSemaphoreCreateCounting(rtos_sem_t *sem,
                                        uint32_t    max_count,
                                        uint32_t    initial_count)
{
    if (!sem || max_count == 0 || initial_count > max_count) return NULL;
    sem->count     = initial_count;
    sem->max_count = max_count;
    sem->wait_list = NULL;
    return (rtos_handle_t)sem;
}

int xSemaphoreTake(rtos_handle_t handle, uint32_t timeout_ticks)
{
    rtos_sem_t *sem = (rtos_sem_t *)handle;
    if (!sem) return RTOS_ERR;

    port_enter_critical();

    if (sem->count > 0) {
        sem->count--;
        port_exit_critical();
        return RTOS_OK;
    }

    // Semaphore not available
    if (timeout_ticks == RTOS_NO_WAIT) {
        port_exit_critical();
        return RTOS_TIMEOUT;
    }

    // Block the current task
    rtos_tcb_t *self = current_task();
    self->state       = TASK_BLOCKED;
    self->delay_ticks = timeout_ticks;
    list_insert_tail(&sem->wait_list, self);
    port_exit_critical();

    port_request_reschedule();

    // When we wake up: either we were given the semaphore (count was decremented
    // for us by xSemaphoreGive) or we timed out.
    port_enter_critical();
    // If still on the wait list we timed out; remove ourselves.
    int on_list = list_remove(&sem->wait_list, self);
    port_exit_critical();

    return on_list ? RTOS_TIMEOUT : RTOS_OK;
}

void xSemaphoreGive(rtos_handle_t handle)
{
    rtos_sem_t *sem = (rtos_sem_t *)handle;
    if (!sem) return;

    port_enter_critical();

    rtos_tcb_t *waiter = list_pop_head(&sem->wait_list);
    if (waiter) {
        // Hand the token directly to the waiter — do not increment count.
        rtos_task_make_ready(waiter);
    } else if (sem->count < sem->max_count) {
        sem->count++;
    }

    port_exit_critical();
    port_request_reschedule();
}

void xSemaphoreGiveFromISR(rtos_handle_t handle)
{
    rtos_sem_t *sem = (rtos_sem_t *)handle;
    if (!sem) return;

    rtos_tcb_t *waiter = list_pop_head(&sem->wait_list);
    if (waiter) {
        rtos_task_make_ready(waiter);
    } else if (sem->count < sem->max_count) {
        sem->count++;
    }
    // Caller is responsible for triggering a reschedule via port_request_reschedule()
    // if a higher-priority task was unblocked.
}

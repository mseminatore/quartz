//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
 //
 // Semaphore API.
 //---------------------------------------------------------------------------
#ifndef RTOS_SEM_H
#define RTOS_SEM_H

#include <stdint.h>
#include "rtos_config.h"
#include "rtos_task.h"

// Semaphore storage — declare as a static variable and pass its address.
typedef struct {
    uint32_t     count;
    uint32_t     max_count;
    rtos_tcb_t  *wait_list;   // tasks blocked waiting to take
} rtos_sem_t;

rtos_handle_t xSemaphoreCreateBinary(rtos_sem_t *sem);
rtos_handle_t xSemaphoreCreateCounting(rtos_sem_t *sem,
                                        uint32_t max_count,
                                        uint32_t initial_count);

// Take (decrement). Returns RTOS_OK on success, RTOS_TIMEOUT if timed out.
int  xSemaphoreTake(rtos_handle_t sem, uint32_t timeout_ticks);

// Give (increment). Unblocks the highest-priority waiter if any.
void xSemaphoreGive(rtos_handle_t sem);

// ISR-safe Give. Does not reschedule; call port_request_reschedule() after if needed.
void xSemaphoreGiveFromISR(rtos_handle_t sem);

#endif // RTOS_SEM_H

// Copyright 2025. All rights reserved.
// Message queue implementation (fixed-size items, static circular buffer).
#include <stdint.h>
#include <string.h>
#include "../include/rtos_queue.h"
#include "list.h"
#include "port.h"

extern rtos_tcb_t  *rtos_next_task(void);
extern rtos_tcb_t **rtos_current_tcb_ptr(void);
extern void         rtos_task_make_ready(rtos_tcb_t *tcb);

#define current_task() (*rtos_current_tcb_ptr())

rtos_handle_t xQueueCreate(rtos_queue_t *queue,
                            void         *buf,
                            size_t        item_size,
                            size_t        capacity)
{
    if (!queue || !buf || item_size == 0 || capacity == 0) return NULL;
    queue->buf       = (uint8_t *)buf;
    queue->item_size = item_size;
    queue->capacity  = capacity;
    queue->count     = 0;
    queue->head      = 0;
    queue->tail      = 0;
    queue->send_wait = NULL;
    queue->recv_wait = NULL;
    return (rtos_handle_t)queue;
}

int xQueueSend(rtos_handle_t handle, const void *item, uint32_t timeout_ticks)
{
    rtos_queue_t *q = (rtos_queue_t *)handle;
    if (!q || !item) return RTOS_ERR;

    port_enter_critical();

    if (q->count < q->capacity) {
        memcpy(q->buf + q->tail * q->item_size, item, q->item_size);
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;

        // Unblock a receiver if one is waiting
        rtos_tcb_t *waiter = list_pop_head(&q->recv_wait);
        if (waiter) rtos_task_make_ready(waiter);

        port_exit_critical();
        port_request_reschedule();
        return RTOS_OK;
    }

    // Queue full
    if (timeout_ticks == RTOS_NO_WAIT) {
        port_exit_critical();
        return RTOS_TIMEOUT;
    }

    rtos_tcb_t *self = current_task();
    self->state       = TASK_BLOCKED;
    self->delay_ticks = timeout_ticks;
    list_insert_tail(&q->send_wait, self);
    port_exit_critical();

    port_request_reschedule();

    port_enter_critical();
    int on_list = list_remove(&q->send_wait, self);
    port_exit_critical();

    return on_list ? RTOS_TIMEOUT : RTOS_OK;
}

int xQueueReceive(rtos_handle_t handle, void *item, uint32_t timeout_ticks)
{
    rtos_queue_t *q = (rtos_queue_t *)handle;
    if (!q || !item) return RTOS_ERR;

    port_enter_critical();

    if (q->count > 0) {
        memcpy(item, q->buf + q->head * q->item_size, q->item_size);
        q->head = (q->head + 1) % q->capacity;
        q->count--;

        // Unblock a sender if one is waiting
        rtos_tcb_t *waiter = list_pop_head(&q->send_wait);
        if (waiter) rtos_task_make_ready(waiter);

        port_exit_critical();
        port_request_reschedule();
        return RTOS_OK;
    }

    // Queue empty
    if (timeout_ticks == RTOS_NO_WAIT) {
        port_exit_critical();
        return RTOS_TIMEOUT;
    }

    rtos_tcb_t *self = current_task();
    self->state       = TASK_BLOCKED;
    self->delay_ticks = timeout_ticks;
    list_insert_tail(&q->recv_wait, self);
    port_exit_critical();

    port_request_reschedule();

    port_enter_critical();
    int on_list = list_remove(&q->recv_wait, self);
    port_exit_critical();

    return on_list ? RTOS_TIMEOUT : RTOS_OK;
}

int xQueueSendFromISR(rtos_handle_t handle, const void *item)
{
    rtos_queue_t *q = (rtos_queue_t *)handle;
    if (!q || !item || q->count >= q->capacity) return RTOS_ERR;

    memcpy(q->buf + q->tail * q->item_size, item, q->item_size);
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;

    rtos_tcb_t *waiter = list_pop_head(&q->recv_wait);
    if (waiter) rtos_task_make_ready(waiter);

    return RTOS_OK;
}

size_t xQueueMessagesWaiting(rtos_handle_t handle)
{
    rtos_queue_t *q = (rtos_queue_t *)handle;
    if (!q) return 0;
    return q->count;
}

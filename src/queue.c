//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Message queue implementation (fixed-size items, static circular buffer).
//---------------------------------------------------------------------------
#include <stdint.h>
#include "../include/rtos_queue.h"
#include "../include/rtos_trace.h"
#include "kmem.h"
#include "list.h"
#include "port.h"

extern rtos_tcb_t  *rtos_next_task(void);
extern rtos_tcb_t **rtos_current_tcb_ptr(void);
extern void         rtos_task_make_ready(rtos_tcb_t *tcb);
extern void         rtos_task_blocked_add(rtos_tcb_t *tcb, rtos_tick_t timeout_ticks);
extern void         rtos_task_blocked_remove(rtos_tcb_t *tcb);

#define current_task() (*rtos_current_tcb_ptr())

//---------------------------------------------------------------------------
// Create a queue. The caller must provide storage for the queue struct and 
// the buffer, which can be on the caller's stack or in static memory. Returns 
// a handle to the queue, or NULL on failure (e.g. invalid parameters). The 
// queue is created empty.
//---------------------------------------------------------------------------
rtos_handle_t rtos_queue_create(rtos_queue_t *queue,
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

//---------------------------------------------------------------------------
// Send an item to the queue. If the queue is not full, copies the item into the
// queue and returns OK. If the queue is full, blocks the current task until either
// space is available (in which case the item is copied and the task returns OK) or
// the timeout expires (in which case the task returns TIMEOUT). If timeout_ticks is
// RTOS_NO_WAIT, do not block and return TIMEOUT immediately if the queue is full.
//---------------------------------------------------------------------------
int rtos_queue_send(rtos_handle_t handle, const void *item, rtos_tick_t timeout_ticks)
{
    rtos_queue_t *q = (rtos_queue_t *)handle;
    if (!q || !item) return RTOS_ERR;

    port_enter_critical();

    if (q->count < q->capacity) {
        rtos_kmemcpy(q->buf + q->tail * q->item_size, item, q->item_size);
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;

        // Unblock a receiver if one is waiting
        rtos_tcb_t *waiter = list_pop_head(&q->recv_wait);
        if (waiter) rtos_task_make_ready(waiter);

        port_exit_critical();
        port_request_reschedule();
        RTOS_TRACE_QUEUE_SEND(q);
        return RTOS_OK;
    }

    // Queue full
    if (timeout_ticks == RTOS_NO_WAIT) {
        port_exit_critical();
        return RTOS_TIMEOUT;
    }

    rtos_tcb_t *self = current_task();
    self->state = TASK_BLOCKED;
    self->ipc_wait = &q->send_wait;
    list_insert_sorted(&q->send_wait, self, self->priority);
    rtos_task_blocked_add(self, timeout_ticks);
    port_exit_critical();

    port_request_reschedule();

    port_enter_critical();
    int on_list = list_remove(&q->send_wait, self);
    if (on_list) {
        rtos_task_blocked_remove(self);
#if RTOS_ENABLE_TASK_NOTIFY
    } else if (self->notif_pending) {
        // Notification woke us, not the queue — treat as timeout.
        // notif_pending is intentionally NOT cleared: it passes through to
        // a subsequent rtos_task_notify_wait() call.
        on_list = 1;
#endif
    } else if (q->count < q->capacity) {
        // We were woken because a receiver made space; complete the send now.
        rtos_kmemcpy(q->buf + q->tail * q->item_size, item, q->item_size);
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;
        RTOS_TRACE_QUEUE_SEND(q);
        // Unblock a receiver that may have started waiting in the meantime.
        rtos_tcb_t *waiter = list_pop_head(&q->recv_wait);
        if (waiter) rtos_task_make_ready(waiter);
    } else {
        // The free slot was filled by another task before we re-entered the
        // critical section. Treat as a failed send (caller should retry).
        on_list = 1;
    }
    self->ipc_wait = NULL;
    port_exit_critical();

    return on_list ? RTOS_TIMEOUT : RTOS_OK;
}

//---------------------------------------------------------------------------
// Receive an item from the queue. If the queue is not empty, copies the item
// from the queue into the provided buffer and returns OK. If the queue is empty,
// blocks the current task until either an item is sent to the queue (in which case
// the item is copied and the task returns OK) or the timeout expires (in which case
// the task returns TIMEOUT). If timeout_ticks is RTOS_NO_WAIT, do not block 
// and return TIMEOUT immediately if the queue is empty.
//---------------------------------------------------------------------------
int rtos_queue_receive(rtos_handle_t handle, void *item, rtos_tick_t timeout_ticks)
{
    rtos_queue_t *q = (rtos_queue_t *)handle;
    if (!q || !item) return RTOS_ERR;

    port_enter_critical();

    if (q->count > 0) {
        rtos_kmemcpy(item, q->buf + q->head * q->item_size, q->item_size);
        q->head = (q->head + 1) % q->capacity;
        q->count--;

        // Unblock a sender if one is waiting
        rtos_tcb_t *waiter = list_pop_head(&q->send_wait);
        if (waiter) rtos_task_make_ready(waiter);

        port_exit_critical();
        port_request_reschedule();
        RTOS_TRACE_QUEUE_RECEIVE(q);
        return RTOS_OK;
    }

    // Queue empty
    if (timeout_ticks == RTOS_NO_WAIT) {
        port_exit_critical();
        return RTOS_TIMEOUT;
    }

    rtos_tcb_t *self = current_task();
    self->state = TASK_BLOCKED;
    self->ipc_wait = &q->recv_wait;
    list_insert_sorted(&q->recv_wait, self, self->priority);
    rtos_task_blocked_add(self, timeout_ticks);
    port_exit_critical();

    port_request_reschedule();

    port_enter_critical();
    int on_list = list_remove(&q->recv_wait, self);
    if (on_list) {
        rtos_task_blocked_remove(self);
#if RTOS_ENABLE_TASK_NOTIFY
    } else if (self->notif_pending) {
        // Notification woke us, not the queue — treat as timeout.
        // notif_pending is intentionally NOT cleared: it passes through to
        // a subsequent rtos_task_notify_wait() call.
        on_list = 1;
#endif
    } else if (q->count > 0) {
        rtos_kmemcpy(item, q->buf + q->head * q->item_size, q->item_size);
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        RTOS_TRACE_QUEUE_RECEIVE(q);
        // Unblock a sender that may have been waiting for space
        rtos_tcb_t *waiter = list_pop_head(&q->send_wait);
        if (waiter) rtos_task_make_ready(waiter);
    }
    self->ipc_wait = NULL;
    port_exit_critical();

    return on_list ? RTOS_TIMEOUT : RTOS_OK;
}

//---------------------------------------------------------------------------
// Send an item to the queue from an ISR. If a receiver is blocked waiting,
// it is made ready and a reschedule is requested. Returns RTOS_ERR if full.
//---------------------------------------------------------------------------
int rtos_queue_send_from_isr(rtos_handle_t handle, const void *item)
{
    rtos_queue_t *q = (rtos_queue_t *)handle;
    if (!q || !item || q->count >= q->capacity) return RTOS_ERR;

    rtos_kmemcpy(q->buf + q->tail * q->item_size, item, q->item_size);
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    RTOS_TRACE_QUEUE_SEND(q);

    rtos_tcb_t *waiter = list_pop_head(&q->recv_wait);
    if (waiter) {
        rtos_task_make_ready(waiter);
        port_request_reschedule();
    }

    return RTOS_OK;
}

//---------------------------------------------------------------------------
// Receive an item from the queue from an ISR. Returns RTOS_ERR if the queue
// is empty. Does not block. Unblocks a waiting sender if one is present and
// requests a reschedule.
//---------------------------------------------------------------------------
int rtos_queue_receive_from_isr(rtos_handle_t handle, void *item)
{
    rtos_queue_t *q = (rtos_queue_t *)handle;
    if (!q || !item || q->count == 0) return RTOS_ERR;

    rtos_kmemcpy(item, q->buf + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    RTOS_TRACE_QUEUE_RECEIVE(q);

    rtos_tcb_t *waiter = list_pop_head(&q->send_wait);
    if (waiter) {
        rtos_task_make_ready(waiter);
        port_request_reschedule();
    }

    return RTOS_OK;
}
size_t rtos_queue_messages_waiting(rtos_handle_t handle)
{
    rtos_queue_t *q = (rtos_queue_t *)handle;
    if (!q) return 0;
    port_enter_critical();
    size_t count = q->count;
    port_exit_critical();
    return count;
}

//---------------------------------------------------------------------------
// Return the number of free slots in the queue.
//---------------------------------------------------------------------------
size_t rtos_queue_spaces_available(rtos_handle_t handle)
{
    rtos_queue_t *q = (rtos_queue_t *)handle;
    if (!q) return 0;
    port_enter_critical();
    size_t free_slots = q->capacity - q->count;
    port_exit_critical();
    return free_slots;
}

//---------------------------------------------------------------------------
// Peek at the head item without removing it. Blocks up to timeout_ticks if
// the queue is empty. Does not unblock any senders (nothing was consumed).
//---------------------------------------------------------------------------
int rtos_queue_peek(rtos_handle_t handle, void *item, rtos_tick_t timeout_ticks)
{
    rtos_queue_t *q = (rtos_queue_t *)handle;
    if (!q || !item) return RTOS_ERR;

    port_enter_critical();

    if (q->count > 0) {
        rtos_kmemcpy(item, q->buf + q->head * q->item_size, q->item_size);
        port_exit_critical();
        return RTOS_OK;
    }

    if (timeout_ticks == RTOS_NO_WAIT) {
        port_exit_critical();
        return RTOS_TIMEOUT;
    }

    // Block on recv_wait: a sender will wake us, but we must NOT consume.
    rtos_tcb_t *self = current_task();
    self->state = TASK_BLOCKED;
    self->ipc_wait = &q->recv_wait;
    list_insert_sorted(&q->recv_wait, self, self->priority);
    rtos_task_blocked_add(self, timeout_ticks);
    port_exit_critical();

    port_request_reschedule();

    port_enter_critical();
    int on_list = list_remove(&q->recv_wait, self);
    if (on_list) {
        rtos_task_blocked_remove(self);
#if RTOS_ENABLE_TASK_NOTIFY
    } else if (self->notif_pending) {
        on_list = 1;
#endif
    } else if (q->count > 0) {
        rtos_kmemcpy(item, q->buf + q->head * q->item_size, q->item_size);
        // Re-arm the next receiver so the visible item still has a consumer
        // path. (We were popped as the receiver but didn't consume; wake any
        // other waiter so they may receive.)
        rtos_tcb_t *next = list_pop_head(&q->recv_wait);
        if (next) rtos_task_make_ready(next);
    } else {
        on_list = 1;
    }
    self->ipc_wait = NULL;
    port_exit_critical();

    return on_list ? RTOS_TIMEOUT : RTOS_OK;
}

//---------------------------------------------------------------------------
// Send to the front of the queue (LIFO). Behaves like rtos_queue_send except
// the new item is placed at the head, becoming the next item to be received.
//---------------------------------------------------------------------------
static void queue_write_front(rtos_queue_t *q, const void *item)
{
    // Move head one slot back (with wrap), then write into that slot.
    q->head = (q->head == 0) ? (q->capacity - 1) : (q->head - 1);
    rtos_kmemcpy(q->buf + q->head * q->item_size, item, q->item_size);
    q->count++;
}

int rtos_queue_send_to_front(rtos_handle_t handle, const void *item, rtos_tick_t timeout_ticks)
{
    rtos_queue_t *q = (rtos_queue_t *)handle;
    if (!q || !item) return RTOS_ERR;

    port_enter_critical();

    if (q->count < q->capacity) {
        queue_write_front(q, item);

        rtos_tcb_t *waiter = list_pop_head(&q->recv_wait);
        if (waiter) rtos_task_make_ready(waiter);

        port_exit_critical();
        port_request_reschedule();
        RTOS_TRACE_QUEUE_SEND(q);
        return RTOS_OK;
    }

    if (timeout_ticks == RTOS_NO_WAIT) {
        port_exit_critical();
        return RTOS_TIMEOUT;
    }

    rtos_tcb_t *self = current_task();
    self->state = TASK_BLOCKED;
    self->ipc_wait = &q->send_wait;
    list_insert_sorted(&q->send_wait, self, self->priority);
    rtos_task_blocked_add(self, timeout_ticks);
    port_exit_critical();

    port_request_reschedule();

    port_enter_critical();
    int on_list = list_remove(&q->send_wait, self);
    if (on_list) {
        rtos_task_blocked_remove(self);
#if RTOS_ENABLE_TASK_NOTIFY
    } else if (self->notif_pending) {
        on_list = 1;
#endif
    } else if (q->count < q->capacity) {
        queue_write_front(q, item);
        RTOS_TRACE_QUEUE_SEND(q);
        rtos_tcb_t *waiter = list_pop_head(&q->recv_wait);
        if (waiter) rtos_task_make_ready(waiter);
    } else {
        on_list = 1;
    }
    self->ipc_wait = NULL;
    port_exit_critical();

    return on_list ? RTOS_TIMEOUT : RTOS_OK;
}

//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Message queue API (fixed-size items, static circular buffer).
//---------------------------------------------------------------------------
#ifndef RTOS_QUEUE_H
#define RTOS_QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include "rtos_config.h"
#include "rtos_task.h"

// Queue storage — declare as a static variable and pass its address.
// The data buffer (buf) must also be static: uint8_t buf[item_size * capacity].
typedef struct {
    uint8_t     *buf;
    size_t       item_size;
    size_t       capacity;    // max number of items
    size_t       count;       // current number of items
    size_t       head;        // read index
    size_t       tail;        // write index
    rtos_tcb_t  *send_wait;   // tasks blocked on Send (queue full)
    rtos_tcb_t  *recv_wait;   // tasks blocked on Receive (queue empty)
} rtos_queue_t;

rtos_handle_t rtos_queue_create(rtos_queue_t *queue,
                                void         *buf,
                                size_t        item_size,
                                size_t        capacity);

// Send an item. Blocks up to timeout_ticks if the queue is full.
int rtos_queue_send(rtos_handle_t queue, const void *item, uint32_t timeout_ticks);

// Receive an item. Blocks up to timeout_ticks if the queue is empty.
int rtos_queue_receive(rtos_handle_t queue, void *item, uint32_t timeout_ticks);

// ISR-safe Send. Returns RTOS_OK or RTOS_ERR (full). Does not block.
int rtos_queue_send_from_isr(rtos_handle_t queue, const void *item);

// Return the number of items currently in the queue.
size_t rtos_queue_messages_waiting(rtos_handle_t queue);

#endif // RTOS_QUEUE_H

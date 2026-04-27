// Copyright 2025. All rights reserved.
//
// producer_consumer — queue demo.
//
// A producer task generates integer values and sends them to a fixed-size
// message queue every 200 ms. A consumer task receives them and prints each
// value. Demonstrates rtos_queue_create / rtos_queue_send / rtos_queue_receive.
//
// Build (RP2040):
//   export PICO_SDK_PATH=~/pico-sdk
//   cmake -B build && cmake --build build
//
// Build (host simulation):
//   cmake -B build && cmake --build build && ./build/sample_producer_consumer

#include "rtos.h"
#include <stdio.h>

// ---------------------------------------------------------------------------
// Shared queue
// ---------------------------------------------------------------------------

#define QUEUE_DEPTH  8

static rtos_queue_t  g_queue;
static uint8_t       g_queue_buf[QUEUE_DEPTH * sizeof(int)];
static rtos_handle_t g_queue_handle;

// ---------------------------------------------------------------------------
// Producer task — sends an incrementing counter every 200 ticks
// ---------------------------------------------------------------------------

static rtos_tcb_t producer_tcb;
static uint32_t   producer_stack[256];

static void producer_task(void *arg)
{
    (void)arg;
    int value = 0;
    uint32_t last_wake = rtos_task_tick_count();
    for (;;) {
        rtos_queue_send(g_queue_handle, &value, RTOS_WAIT_FOREVER);
        value++;
        rtos_task_delay_until(&last_wake, 200);   // 200 ms, drift-free send rate
    }
}

// ---------------------------------------------------------------------------
// Consumer task — receives and prints each item
// ---------------------------------------------------------------------------

static rtos_tcb_t consumer_tcb;
static uint32_t   consumer_stack[256];

static void consumer_task(void *arg)
{
    (void)arg;
    int received;
    for (;;) {
        rtos_queue_receive(g_queue_handle, &received, RTOS_WAIT_FOREVER);
        printf("received: %d\n", received);
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(void)
{
    g_queue_handle = rtos_queue_create(&g_queue, g_queue_buf, sizeof(int), QUEUE_DEPTH);

    // Producer at lower priority so consumer can drain before next item arrives
    rtos_task_create(&producer_tcb, producer_stack, 256,
                     producer_task, NULL, "producer", 2);
    rtos_task_create(&consumer_tcb, consumer_stack, 256,
                     consumer_task, NULL, "consumer", 1);

    rtos_start();   // never returns
}

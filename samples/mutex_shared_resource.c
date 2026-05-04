// Copyright 2025. All rights reserved.
//
// mutex_shared_resource — mutex demo.
//
// Two tasks share a counter protected by a mutex. Each task locks the mutex,
// increments the counter, prints it, then unlocks. Without the mutex the
// counter would be subject to a race condition.
//
// Demonstrates rtos_mutex_create / rtos_mutex_lock / rtos_mutex_unlock.
//
// Build (RP2040):
//   export PICO_SDK_PATH=~/pico-sdk
//   cmake -B build && cmake --build build
//
// Build (host simulation):
//   cmake -B build && cmake --build build && ./build/sample_mutex_shared_resource

#include "rtos.h"
#include <stdio.h>

// ---------------------------------------------------------------------------
// Shared resource
// ---------------------------------------------------------------------------

static rtos_mutex_t  g_mutex;
static rtos_handle_t g_mutex_handle;
static int           g_counter = 0;

// ---------------------------------------------------------------------------
// Worker task — acquires the mutex, increments the counter, releases it
// ---------------------------------------------------------------------------

static void worker_task(void *arg)
{
    const char *name = (const char *)arg;
    for (;;) {
        rtos_mutex_lock(g_mutex_handle, RTOS_WAIT_FOREVER);

        g_counter++;
        printf("[%s] counter = %d\n", name, g_counter);

        rtos_mutex_unlock(g_mutex_handle);

        rtos_task_delay(300);
    }
}

// ---------------------------------------------------------------------------
// Task storage
// ---------------------------------------------------------------------------

static rtos_tcb_t worker1_tcb;
static rtos_stack_t   worker1_stack[256];

static rtos_tcb_t worker2_tcb;
static rtos_stack_t   worker2_stack[256];

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(void)
{
    g_mutex_handle = rtos_mutex_create(&g_mutex);

    rtos_task_create(&worker1_tcb, worker1_stack, 256,
                     worker_task, "worker1", "worker1", 1);
    rtos_task_create(&worker2_tcb, worker2_stack, 256,
                     worker_task, "worker2", "worker2", 1);

    rtos_start();   // never returns
}

// Copyright 2025. All rights reserved.
//
// trace_demo — exercises the chrome-trace recorder.
//
// Two tasks share a semaphore + queue + mutex so that switch_in/switch_out
// and sem/queue/mutex events are all produced.  After ITERATIONS handshakes
// the consumer drains the trace ring buffer to stdout in ASCII hex.
// The host script tools/trace_to_chrome.py converts that hex stream to a
// Chrome Trace Event Format JSON file.
//
// Build (host simulation, with the chrome backend enabled):
//   cmake -B build -DRTOS_TRACE_BACKEND=chrome
//   cmake --build build --target sample_trace_demo
//   ./build/sample_trace_demo > capture.hex
//   python3 tools/trace_to_chrome.py capture.hex --hex -o trace.json
//
// NOTE: the host port (port/host/port.c) uses POSIX <ucontext.h>, so this
// demo builds on Linux, macOS, or WSL — but NOT with native MSVC on
// Windows.  Windows users should run the commands above from a WSL shell.
// See README.md "Tracing & visualization" for a step-by-step WSL recipe.

#include "rtos.h"
#include "rtos_trace_chrome.h"
#include <stdio.h>
#include <stdlib.h>

#define ITERATIONS  20

static rtos_tcb_t      g_producer_tcb, g_consumer_tcb;
static uint32_t        g_producer_stack[256];
static uint32_t        g_consumer_stack[256];

static rtos_sem_t      g_sem;
static rtos_handle_t   g_sem_h;

static rtos_mutex_t    g_mutex;
static rtos_handle_t   g_mutex_h;

static rtos_queue_t    g_queue;
static uint8_t         g_queue_buf[8 * sizeof(int)];
static rtos_handle_t   g_queue_h;

static int hex_putch(int c, void *ctx)
{
    (void)ctx;
    return fputc(c, stdout);
}

static void producer_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERATIONS; ++i) {
        rtos_mutex_lock(g_mutex_h, RTOS_WAIT_FOREVER);
        rtos_queue_send(g_queue_h, &i, RTOS_WAIT_FOREVER);
        rtos_mutex_unlock(g_mutex_h);
        rtos_semaphore_give(g_sem_h);
        rtos_task_delay(2);
    }
    rtos_task_suspend(NULL);
}

static void consumer_task(void *arg)
{
    (void)arg;
    for (int got = 0; got < ITERATIONS; ++got) {
        rtos_semaphore_take(g_sem_h, RTOS_WAIT_FOREVER);
        int v;
        rtos_queue_receive(g_queue_h, &v, RTOS_WAIT_FOREVER);
    }

    fprintf(stderr,
            "trace_demo: captured %lu records (overflow=%d)\n",
            (unsigned long)rtos_trace_chrome_count(),
            rtos_trace_chrome_overflowed());

    rtos_trace_chrome_dump_hex(hex_putch, NULL);
    fflush(stdout);
    exit(0);   // host simulation has no shutdown API
}

int main(void)
{
    g_sem_h   = rtos_semaphore_create_counting(&g_sem, 16, 0);
    g_mutex_h = rtos_mutex_create(&g_mutex);
    g_queue_h = rtos_queue_create(&g_queue, g_queue_buf, sizeof(int), 8);

    rtos_task_create(&g_producer_tcb, g_producer_stack, 256,
                     producer_task, NULL, "producer", 2);
    rtos_task_create(&g_consumer_tcb, g_consumer_stack, 256,
                     consumer_task, NULL, "consumer", 1);

    rtos_start();
    return 0;
}


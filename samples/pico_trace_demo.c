// Copyright 2025. All rights reserved.
//
// pico_trace_demo — exercises the chrome-trace recorder on RP2040 / Pico W.
//
// Two tasks share a semaphore + queue + mutex so that switch_in/switch_out
// and sem/queue/mutex events are all produced.  After ITERATIONS handshakes
// the consumer drains the trace ring buffer over USB CDC as ASCII hex,
// framed by ---BEGIN-TRACE--- / ---END--- markers so the host can find it
// even if the serial log captures unrelated boot chatter.
//
// Build (RP2040 / Pico / Pico W) with the chrome backend enabled:
//   export PICO_SDK_PATH=~/pico-sdk
//   cmake -B build_pico -DPICO_BOARD=pico_w -DRTOS_TRACE_BACKEND=chrome
//   cmake --build build_pico --target sample_pico_trace_demo
//   # Flash via picoprobe / Pi Debug Probe (SWD) or BOOTSEL+drag-drop:
//   picotool load -fx build_pico/sample_pico_trace_demo.uf2
//
// Capture on the host (USB CDC enumerates on the Pico's native USB port —
// the debug probe is only used for flashing / SWD):
//   # Linux / macOS / WSL2:
//   cat /dev/ttyACM0 > capture.txt &
//   sleep 15 && pkill cat
//   # Windows: open the COM port in PuTTY with logging enabled.
//
// Decode and view:
//   python3 tools/trace_to_chrome.py capture.txt --hex -o trace.json
//   # then open trace.json in chrome://tracing or https://ui.perfetto.dev

#include "rtos.h"
#include "rtos_trace_chrome.h"
#include "pico/stdlib.h"
#include <stdio.h>

#define ITERATIONS  20

static rtos_tcb_t      g_producer_tcb, g_consumer_tcb;
static rtos_stack_t        g_producer_stack[256];
static rtos_stack_t        g_consumer_stack[256];

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
    return putchar(c);
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

    printf("\ntrace_demo: captured %lu records (overflow=%d)\n",
           (unsigned long)rtos_trace_chrome_count(),
           rtos_trace_chrome_overflowed());
    printf("---BEGIN-TRACE---\n");
    rtos_trace_chrome_dump_hex(hex_putch, NULL);
    printf("\n---END---\n");

    // Park forever — leaves the device idle so a follow-up reset/reflash works.
    rtos_task_suspend(NULL);
}

int main(void)
{
    stdio_init_all();
    sleep_ms(2000);   // give USB CDC time to enumerate before we print

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

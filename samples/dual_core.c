//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// dual_core — RP2040 dual-core AMP (Asymmetric Multi-Processing) demo.
//
// Distributes RTOS tasks across both Cortex-M0+ cores of the RP2040:
//
//   Core 0 (rtos_start):
//     producer_task — sends incrementing integers to a shared queue every 50 ms
//     stats_task    — prints throughput statistics every 2 s
//
//   Core 1 (multicore_launch_core1 → rtos_core1_entry):
//     consumer_task — receives items from the queue, accumulates a running total
//
// The queue is the sole inter-core communication channel.  The RTOS's
// spinlock-backed critical sections ensure safe concurrent access from both
// cores.
//
// Sequence of events in main():
//   1. Create the shared queue.
//   2. Create producer and stats tasks on core 0 (rtos_task_create — defaults
//      to the calling core, which is core 0 at startup).
//   3. Create the consumer task pinned to core 1 (rtos_task_create_on_core).
//   4. Launch core 1 with multicore_launch_core1(rtos_core1_entry).
//      rtos_core1_entry() configures core 1's SysTick and starts its scheduler.
//   5. Call rtos_start() on core 0.
//
// Build (RP2040):
//   export PICO_SDK_PATH=~/pico-sdk
//   cmake -B build_pico && cmake --build build_pico
//   # Flash build_pico/sample_dual_core.uf2; open USB serial to see output.
//
// NOTE: This sample requires RTOS_NUM_CORES=2 (set by CMakeLists.txt) and the
//       pico_multicore Pico SDK library.  It is NOT available in the host
//       simulation build (single-core only).
//---------------------------------------------------------------------------

#include "rtos.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include <stdio.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Shared queue — producer (core 0) → consumer (core 1)
// ---------------------------------------------------------------------------

#define QUEUE_DEPTH  16

static rtos_queue_t  g_queue;
static uint8_t       g_queue_buf[QUEUE_DEPTH * sizeof(uint32_t)];
static rtos_handle_t g_queue_h;

// ---------------------------------------------------------------------------
// Stats shared between consumer and stats tasks (read from core 0)
// Updated by consumer (core 1) — volatile for cross-core visibility
// ---------------------------------------------------------------------------

static volatile uint32_t g_total_received = 0;
static volatile uint32_t g_last_value     = 0;

// ---------------------------------------------------------------------------
// Producer task (core 0) — sends incrementing integers every 50 ms
// ---------------------------------------------------------------------------

static rtos_tcb_t   g_producer_tcb;
static rtos_stack_t g_producer_stack[256];

static void producer_task(void *arg)
{
    (void)arg;
    uint32_t    value     = 0;
    rtos_tick_t last_wake = rtos_task_tick_count();

    for (;;) {
        rtos_queue_send(g_queue_h, &value, RTOS_NO_WAIT);  // drop on queue full
        value++;
        rtos_task_delay_until(&last_wake, 50);   // 50 ms, drift-free
    }
}

// ---------------------------------------------------------------------------
// Consumer task (core 1) — receives items and accumulates a running total
// ---------------------------------------------------------------------------

static rtos_tcb_t   g_consumer_tcb;
static rtos_stack_t g_consumer_stack[256];

static void consumer_task(void *arg)
{
    (void)arg;
    uint32_t v;

    for (;;) {
        if (rtos_queue_receive(g_queue_h, &v, RTOS_WAIT_FOREVER) == RTOS_OK) {
            g_total_received++;
            g_last_value = v;
        }
    }
}

// ---------------------------------------------------------------------------
// Stats task (core 0) — prints throughput every 2 s
// ---------------------------------------------------------------------------

static rtos_tcb_t   g_stats_tcb;
static rtos_stack_t g_stats_stack[256];

static void stats_task(void *arg)
{
    (void)arg;
    uint32_t    last_received = 0;
    rtos_tick_t last_wake     = rtos_task_tick_count();

    for (;;) {
        rtos_task_delay_until(&last_wake, 2000);

        uint32_t total = g_total_received;
        uint32_t delta = total - last_received;
        last_received  = total;

        printf("[dual_core] last_value=%-6lu  total_received=%-8lu  rate=%lu/2s\n",
               (unsigned long)g_last_value,
               (unsigned long)total,
               (unsigned long)delta);
    }
}

//---------------------------------------------------------------------------
// Entry point
//---------------------------------------------------------------------------
int main(void)
{
    stdio_init_all();
    sleep_ms(1000);   // wait for USB CDC to enumerate

    g_queue_h = rtos_queue_create(&g_queue, g_queue_buf,
                                   sizeof(uint32_t), QUEUE_DEPTH);

    // Core 0 tasks — created from core 0, so they default to core 0
    rtos_task_create(&g_producer_tcb, g_producer_stack, 256,
                     producer_task, NULL, "producer", 1);
    rtos_task_create(&g_stats_tcb, g_stats_stack, 256,
                     stats_task, NULL, "stats", 2);

    // Core 1 task — pinned to core 1
    rtos_task_create_on_core(&g_consumer_tcb, g_consumer_stack, 256,
                              consumer_task, NULL, "consumer", 1, /*core=*/1);

    // Launch core 1 scheduler — rtos_core1_entry configures SysTick on core 1
    // and starts scheduling.  Must be called BEFORE rtos_start() on core 0.
    multicore_launch_core1(rtos_core1_entry);

    rtos_start();   // starts core 0 scheduler; never returns
    return 0;
}

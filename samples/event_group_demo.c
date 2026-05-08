//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// event_group_demo — fan-in synchronization with event groups.
//
// Three tasks cooperate using a single event group:
//
//   sensor_task  — simulates a slow sensor read (200 ms); sets bit 0x01 when
//                  data is ready, then waits for the next cycle.
//   comms_task   — simulates a network/UART link check (350 ms); sets bit
//                  0x02 when the link is up, then waits for the next cycle.
//   process_task — blocks on ALL of {0x01, 0x02} with clear_on_exit so the
//                  event group resets automatically.  Prints a message and
//                  records the latency (ticks from cycle start to wake).
//
// This pattern — multiple producers signalling a single consumer using
// event groups — cannot be done cleanly with a single semaphore: a counting
// semaphore could wake too early (on the first give, not both), and two
// separate semaphores require two blocking calls that may run out of order.
// Event groups solve this natively.
//
// Expected output (approximate, 1 tick = 1 ms):
//
//   [cycle   1] sensor ready after  200 ms, comms ready after  350 ms
//   [cycle   1] processing! woke after 350 ms (both bits set at tick  350)
//   [cycle   2] sensor ready after  200 ms, comms ready after  350 ms
//   ...
//
// Build (host simulation):
//   cmake -B build && cmake --build build && ./build/sample_event_group_demo
//
// Build (RP2040 / Pico):
//   cmake -B build_pico && cmake --build build_pico
//   # Flash build_pico/sample_event_group_demo.uf2 via BOOTSEL.
//---------------------------------------------------------------------------

#include "rtos.h"
#include <stdio.h>
#include <stdint.h>

#ifdef __rp2040__
#   include "pico/stdlib.h"
#endif

#if !RTOS_ENABLE_EVENT_GROUPS
#   error "This sample requires RTOS_ENABLE_EVENT_GROUPS=1"
#endif

// ---------------------------------------------------------------------------
// Shared event group
// ---------------------------------------------------------------------------

static rtos_eventgroup_t g_events;
static rtos_handle_t     g_event_handle;

#define BIT_SENSOR  0x01u
#define BIT_COMMS   0x02u

// ---------------------------------------------------------------------------
// sensor_task — signals BIT_SENSOR every SENSOR_PERIOD_MS milliseconds
// ---------------------------------------------------------------------------

static rtos_tcb_t   g_sensor_tcb;
static rtos_stack_t g_sensor_stack[256];

#define SENSOR_PERIOD_MS  200u

static void sensor_task(void *arg)
{
    (void)arg;
    rtos_tick_t last_wake = rtos_task_tick_count();
    uint32_t    cycle     = 0;

    for (;;) {
        rtos_task_delay_until(&last_wake, SENSOR_PERIOD_MS);
        cycle++;
        printf("[cycle %3u] sensor ready after %4u ms\n",
               cycle, (unsigned)(SENSOR_PERIOD_MS));
        rtos_eventgroup_set(g_event_handle, BIT_SENSOR);
    }
}

// ---------------------------------------------------------------------------
// comms_task — signals BIT_COMMS every COMMS_PERIOD_MS milliseconds
// ---------------------------------------------------------------------------

static rtos_tcb_t   g_comms_tcb;
static rtos_stack_t g_comms_stack[256];

#define COMMS_PERIOD_MS   350u

static void comms_task(void *arg)
{
    (void)arg;
    rtos_tick_t last_wake = rtos_task_tick_count();
    uint32_t    cycle     = 0;

    for (;;) {
        rtos_task_delay_until(&last_wake, COMMS_PERIOD_MS);
        cycle++;
        printf("[cycle %3u] comms  ready after %4u ms\n",
               cycle, (unsigned)(COMMS_PERIOD_MS));
        rtos_eventgroup_set(g_event_handle, BIT_COMMS);
    }
}

// ---------------------------------------------------------------------------
// process_task — waits for BOTH sensor AND comms to be ready
// ---------------------------------------------------------------------------

static rtos_tcb_t   g_process_tcb;
static rtos_stack_t g_process_stack[512];

static void process_task(void *arg)
{
    (void)arg;
    rtos_tick_t cycle_start = rtos_task_tick_count();
    uint32_t    cycle       = 0;

    for (;;) {
        // Block until BOTH bits are set; auto-clear so the group resets.
        uint32_t bits = rtos_eventgroup_wait(g_event_handle,
                                              BIT_SENSOR | BIT_COMMS,
                                              RTOS_EG_WAIT_ALL,    // AND
                                              1,                    // clear_on_exit
                                              RTOS_WAIT_FOREVER);
        cycle++;
        rtos_tick_t now     = rtos_task_tick_count();
        rtos_tick_t latency = now - cycle_start;
        cycle_start         = now;

        printf("[cycle %3u] processing! woke after %4lu ms "
               "(bits=0x%02lx at tick %5lu)\n",
               cycle,
               (unsigned long)latency,
               (unsigned long)bits,
               (unsigned long)now);
    }
}

//---------------------------------------------------------------------------
// Entry point
//---------------------------------------------------------------------------
int main(void)
{
#ifdef __rp2040__
    stdio_init_all();
#endif

    g_event_handle = rtos_eventgroup_create(&g_events);

    // process_task has highest priority so it runs immediately when woken.
    rtos_task_create(&g_process_tcb, g_process_stack, 512,
                     process_task, NULL, "process", 1);

    // sensor and comms tasks are equal priority — they run concurrently.
    rtos_task_create(&g_sensor_tcb, g_sensor_stack, 256,
                     sensor_task, NULL, "sensor", 2);

    rtos_task_create(&g_comms_tcb, g_comms_stack, 256,
                     comms_task, NULL, "comms", 2);

    rtos_start();   // never returns
    return 0;
}

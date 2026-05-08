//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// timer_demo — software timer demo.
//
// Two software timers are created and exercised:
//
//   heartbeat — periodic 250 ms; callback increments a counter.
//               After 10 s the control task stops it for 3 s, then restarts it
//               to demonstrate rtos_timer_stop / rtos_timer_start.
//
//   alarm     — one-shot 4 s; fires once (auto-deactivates) then the control
//               task calls rtos_timer_reset() to restart the countdown.
//
// A control task prints a summary every second showing how many heartbeat
// ticks have accumulated and whether the alarm has fired.
//
// Demonstrates: rtos_timer_create / rtos_timer_start / rtos_timer_stop /
//               rtos_timer_reset / rtos_timer_is_active
//
// Expected output (first 12 seconds, approximate):
//
//   [timer] t= 1  heartbeat: 4 (+4)   alarm fires: 0  hb_active: 1  alarm_active: 1
//   [timer] t= 2  heartbeat: 8 (+4)   alarm fires: 0  hb_active: 1  alarm_active: 1
//   ...
//   [timer] t= 4  heartbeat: 16 (+4)  alarm fires: 1  hb_active: 1  alarm_active: 0
//   [timer] alarm fired — resetting alarm for another 4000 ms
//   ...
//   [timer] t=10  stopping heartbeat for 3 s
//   [timer] t=11  heartbeat: 40 (+0)  alarm fires: 3  hb_active: 0  alarm_active: 1
//   ...
//   [timer] t=13  restarting heartbeat
//
// Build (host simulation):
//   cmake -B build && cmake --build build && ./build/sample_timer_demo
//
// Build (RP2040 / Pico W):
//   export PICO_SDK_PATH=~/pico-sdk
//   cmake -B build_pico && cmake --build build_pico
//---------------------------------------------------------------------------

#include "rtos.h"
#include <stdio.h>

#ifdef __rp2040__
#   include "pico/stdlib.h"
#endif

// ---------------------------------------------------------------------------
// Timer storage
// ---------------------------------------------------------------------------

static rtos_timer_t  g_heartbeat_timer;
static rtos_handle_t g_heartbeat_h;

static rtos_timer_t  g_alarm_timer;
static rtos_handle_t g_alarm_h;

// ---------------------------------------------------------------------------
// Shared state updated by timer callbacks (ISR context — volatile)
// ---------------------------------------------------------------------------

static volatile uint32_t g_heartbeat_count = 0;   // incremented each 250 ms tick
static volatile uint32_t g_alarm_fires     = 0;   // incremented each time alarm fires

// ---------------------------------------------------------------------------
// Timer callbacks — called from the tick ISR; must be short and non-blocking
// ---------------------------------------------------------------------------

static void heartbeat_cb(rtos_handle_t timer)
{
    (void)timer;
    g_heartbeat_count++;
}

static void alarm_cb(rtos_handle_t timer)
{
    (void)timer;
    g_alarm_fires++;
    // one-shot: auto-deactivates after this call returns
}

// ---------------------------------------------------------------------------
// Control task
// ---------------------------------------------------------------------------

static rtos_tcb_t   g_ctrl_tcb;
static rtos_stack_t g_ctrl_stack[256];

#define STOP_AT_SECOND   10   // stop heartbeat at this elapsed second
#define RESUME_AFTER      3   // restart heartbeat this many seconds later

static void ctrl_task(void *arg)
{
    (void)arg;

    rtos_timer_start(g_heartbeat_h);
    rtos_timer_start(g_alarm_h);

    uint32_t last_hb      = 0;
    uint32_t last_alarm   = 0;
    int      elapsed_s    = 0;
    int      stopped      = 0;
    int      stop_at      = -1;

    rtos_tick_t last_wake = rtos_task_tick_count();

    for (;;) {
        rtos_task_delay_until(&last_wake, 1000);   // run once per second
        elapsed_s++;

        uint32_t hb    = g_heartbeat_count;
        uint32_t fires = g_alarm_fires;

        printf("[timer] t=%2d  heartbeat: %3lu (+%lu)  "
               "alarm fires: %lu  hb_active: %d  alarm_active: %d\n",
               elapsed_s,
               (unsigned long)hb,
               (unsigned long)(hb - last_hb),
               (unsigned long)fires,
               rtos_timer_is_active(g_heartbeat_h),
               rtos_timer_is_active(g_alarm_h));

        // When the alarm fires, reset it to restart the countdown from scratch
        if (fires != last_alarm) {
            printf("[timer] alarm fired — resetting alarm for another 4000 ms\n");
            rtos_timer_reset(g_alarm_h);
        }

        // Stop the heartbeat at STOP_AT_SECOND seconds
        if (!stopped && elapsed_s == STOP_AT_SECOND) {
            printf("[timer] stopping heartbeat for %d s\n", RESUME_AFTER);
            rtos_timer_stop(g_heartbeat_h);
            stop_at = elapsed_s;
            stopped = 1;
        }

        // Restart the heartbeat after RESUME_AFTER seconds
        if (stopped && (elapsed_s - stop_at) == RESUME_AFTER) {
            printf("[timer] restarting heartbeat\n");
            rtos_timer_start(g_heartbeat_h);
            stopped = 0;
        }

        last_hb    = hb;
        last_alarm = fires;
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

    // Periodic 250 ms heartbeat
    g_heartbeat_h = rtos_timer_create(&g_heartbeat_timer, "heartbeat",
                                       250, /*periodic=*/1, heartbeat_cb);

    // One-shot 4 s alarm (re-armed by the control task each time it fires)
    g_alarm_h = rtos_timer_create(&g_alarm_timer, "alarm",
                                   4000, /*periodic=*/0, alarm_cb);

    rtos_task_create(&g_ctrl_tcb, g_ctrl_stack, 256,
                     ctrl_task, NULL, "ctrl", 1);

    rtos_start();   // never returns
    return 0;
}

//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// blink — minimal single-task demo.
//
// One task toggles an LED every 500 ms using rtos_task_delay_until() for a
// drift-free period.  rtos_task_delay_until() accounts for the time spent in
// the task body so the LED period is always exactly 500 ticks regardless of
// scheduling jitter or the cost of the GPIO call.
// On RP2040 it drives GPIO 25 (the on-board LED).
// On the host build the GPIO calls are stubbed so the file compiles and
// the task loop runs in simulation.
//
// A second "monitor" task verifies timing accuracy at runtime by recording
// rtos_task_tick_count() at each LED toggle and printing period statistics
// every 10 toggles.  Expected output (ideal):
//
//   [blink-monitor] periods 1-10: min=500 max=500 avg=500 drift=0 ticks
//
// On real hardware (RP2040), any inaccuracy in the SysTick calibration or
// interrupt latency appears here.  See also:
//
//   Hardware measurement — connect GPIO 25 to an oscilloscope and trigger
//   on rising edges.  The full LED cycle (on→off→on) should measure exactly
//   1000 ms.  At 1 kHz tick rate, one tick = 1 ms, so 500 ticks = 500 ms.
//   A logic analyser (e.g. Saleae) can also measure the period to sub-ms
//   resolution and log hundreds of consecutive cycles to detect jitter.
//
// Build (RP2040):
//   export PICO_SDK_PATH=~/pico-sdk
//   cmake -B build && cmake --build build
//
// Build (host simulation):
//   cmake -B build && cmake --build build && ./build/sample_blink
//---------------------------------------------------------------------------

#include "rtos.h"
#include <stdio.h>

// ---------------------------------------------------------------------------
// Hardware abstraction — real GPIO on RP2040, print stubs on host
// ---------------------------------------------------------------------------

#ifdef __rp2040__
#   include "pico/stdlib.h"
#   define LED_PIN  25
#   define hw_init()         do { gpio_init(LED_PIN); gpio_set_dir(LED_PIN, GPIO_OUT); } while(0)
#   define hw_led_set(v)     gpio_put(LED_PIN, (v))
#else
#   define hw_init()         do {} while(0)
#   define hw_led_set(v)     printf("LED %s\n", (v) ? "ON" : "OFF")
#endif

// ---------------------------------------------------------------------------
// Shared state between blink task and monitor task
// ---------------------------------------------------------------------------

static volatile rtos_tick_t g_toggle_tick = 0;   /* tick of last LED toggle */
static volatile int         g_toggle_count = 0;  /* total toggles so far    */

// ---------------------------------------------------------------------------
// Blink task — drift-free 500 ms LED toggle
// ---------------------------------------------------------------------------

static rtos_tcb_t  blink_tcb;
static uint32_t    blink_stack[256];

static void blink_task(void *arg)
{
    (void)arg;
    int state = 0;
    rtos_tick_t last_wake = rtos_task_tick_count();

    for (;;)
    {
        state ^= 1;
        hw_led_set(state);
        g_toggle_tick = rtos_task_tick_count();
        g_toggle_count++;                         /* record toggle for monitor */
        rtos_task_delay_until(&last_wake, 500);   /* 500 ticks = 500 ms, drift-free */
    }
}

// ---------------------------------------------------------------------------
// Monitor task — measures and prints period accuracy every 10 toggles
// ---------------------------------------------------------------------------

static rtos_tcb_t  monitor_tcb;
static uint32_t    monitor_stack[256];

#define MONITOR_WINDOW  10          /* print stats every N toggles */

static void monitor_task(void *arg)
{
    (void)arg;

    int         last_count   = 0;
    rtos_tick_t last_tick    = 0;
    rtos_tick_t period_min   = (rtos_tick_t)-1;
    rtos_tick_t period_max   = 0;
    rtos_tick_t period_sum   = 0;
    int         window_count = 0;
    int         batch        = 1;

    /* Wait for the first toggle before recording */
    while (g_toggle_count == 0)
        rtos_task_delay(1);

    last_count = g_toggle_count;
    last_tick  = g_toggle_tick;

    for (;;)
    {
        /* Poll until blink_task records a new toggle */
        rtos_task_delay(1);
        int   cur_count = g_toggle_count;
        if (cur_count == last_count)
            continue;

        rtos_tick_t cur_tick = g_toggle_tick;
        rtos_tick_t period   = cur_tick - last_tick;

        if (period < period_min) period_min = period;
        if (period > period_max) period_max = period;
        period_sum += period;
        window_count++;

        last_count = cur_count;
        last_tick  = cur_tick;

        if (window_count == MONITOR_WINDOW)
        {
            rtos_tick_t avg      = period_sum / (rtos_tick_t)MONITOR_WINDOW;
            rtos_tick_t expected = 500u * (rtos_tick_t)MONITOR_WINDOW;
            rtos_tick_t actual   = period_sum;
            /* signed drift: positive = running slow, negative = running fast */
            int32_t     drift    = (int32_t)(actual - expected);

            printf("[blink-monitor] periods %d-%d: min=%lu max=%lu avg=%lu drift=%ld ticks\n",
                   (batch - 1) * MONITOR_WINDOW + 1,
                   batch * MONITOR_WINDOW,
                   (unsigned long)period_min,
                   (unsigned long)period_max,
                   (unsigned long)avg,
                   (long)drift);

            period_min   = (rtos_tick_t)-1;
            period_max   = 0;
            period_sum   = 0;
            window_count = 0;
            batch++;
        }
    }
}

//---------------------------------------------------------------------------
// Entry point
//---------------------------------------------------------------------------
int main(void)
{
    hw_init();

    rtos_task_create(&blink_tcb, blink_stack, 256,
                     blink_task, NULL, "blink", 1);

    rtos_task_create(&monitor_tcb, monitor_stack, 256,
                     monitor_task, NULL, "monitor", 2);

    rtos_start();   // never returns
}

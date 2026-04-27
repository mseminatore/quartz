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
// Task
// ---------------------------------------------------------------------------

static rtos_tcb_t  blink_tcb;
static uint32_t    blink_stack[256];

static void blink_task(void *arg)
{
    (void)arg;
    int state = 0;
    uint32_t last_wake = rtos_task_tick_count();

    for (;;)
    {
        state ^= 1;
        hw_led_set(state);
        rtos_task_delay_until(&last_wake, 500);   // 500 ticks = 500 ms, drift-free
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

    rtos_start();   // never returns
}

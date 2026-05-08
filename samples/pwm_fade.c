//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// pwm_fade — PWM LED fade demo with task suspend/resume.
//
// Two tasks run concurrently:
//
//   fade_task  — ramps the on-board LED duty cycle 0→100%→0% over ~4 s
//                using rtos_task_delay_until() for a drift-free period.
//
//   pause_task — every 10 s, suspends fade_task for 2 s (LED holds its
//                current brightness), then resumes it.
//                Demonstrates rtos_task_suspend() / rtos_task_resume().
//
// Hardware: standard Raspberry Pi Pico (not Pico W).
//   - The on-board LED is connected to GP25, which is a PWM-capable pin.
//   - On Pico W the LED is wired to the CYW43 chip (not GP25), so PWM
//     will not drive it.  Connect an external LED + resistor to GP25 for
//     Pico W, or adapt the pin number to another GPIO.
//
// Build (RP2040 / Pico):
//   export PICO_SDK_PATH=~/pico-sdk
//   cmake -B build_pico && cmake --build build_pico
//   # Flash build_pico/sample_pwm_fade.uf2 via BOOTSEL or picoprobe.
//   # The LED fades in and out; every 10 s it pauses for 2 s.
//---------------------------------------------------------------------------

#include "rtos.h"
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include <stdio.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// LED pin and PWM configuration
// ---------------------------------------------------------------------------

#define LED_PIN    25u         // GP25 — on-board LED on standard Pico
#define PWM_WRAP   999u        // duty cycle range: 0 .. PWM_WRAP (inclusive)

// ---------------------------------------------------------------------------
// Shared fade task handle — used by pause_task to suspend/resume it
// ---------------------------------------------------------------------------

static rtos_handle_t g_fade_handle;

// ---------------------------------------------------------------------------
// fade_task — ramps duty cycle 0→PWM_WRAP→0 in STEP_MS increments
// ---------------------------------------------------------------------------

static rtos_tcb_t   g_fade_tcb;
static rtos_stack_t g_fade_stack[256];

#define STEP_MS   4u          // delay between duty-cycle steps (ms)
                               // 1000 steps × 4 ms × 2 directions = 8 s full cycle

static void fade_task(void *arg)
{
    (void)arg;

    uint32_t    level     = 0;
    int         direction = 1;          // +1 = brightening, -1 = dimming
    rtos_tick_t last_wake = rtos_task_tick_count();

    for (;;) {
        pwm_set_gpio_level(LED_PIN, level);

        // Advance duty cycle
        if (direction > 0) {
            if (level < PWM_WRAP) {
                level++;
            } else {
                direction = -1;
            }
        } else {
            if (level > 0) {
                level--;
            } else {
                direction = 1;
            }
        }

        rtos_task_delay_until(&last_wake, STEP_MS);
    }
}

// ---------------------------------------------------------------------------
// pause_task — suspends the fade task for 2 s every 10 s
// ---------------------------------------------------------------------------

static rtos_tcb_t   g_pause_tcb;
static rtos_stack_t g_pause_stack[256];

#define PAUSE_INTERVAL_MS  10000u   // ms between pauses
#define PAUSE_DURATION_MS   2000u   // ms to hold the LED at its current level

static void pause_task(void *arg)
{
    (void)arg;
    rtos_tick_t last_wake = rtos_task_tick_count();

    for (;;) {
        rtos_task_delay_until(&last_wake, PAUSE_INTERVAL_MS);

        printf("[pwm_fade] pausing fade for %lu ms\n", (unsigned long)PAUSE_DURATION_MS);
        rtos_task_suspend(g_fade_handle);

        rtos_task_delay(PAUSE_DURATION_MS);

        printf("[pwm_fade] resuming fade\n");
        rtos_task_resume(g_fade_handle);
    }
}

//---------------------------------------------------------------------------
// Entry point
//---------------------------------------------------------------------------
int main(void)
{
    stdio_init_all();

    // Configure GP25 as a PWM output
    gpio_set_function(LED_PIN, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(LED_PIN);
    pwm_set_wrap(slice, PWM_WRAP);
    pwm_set_gpio_level(LED_PIN, 0);
    pwm_set_enabled(slice, true);

    // Create fade task (higher priority so it runs on schedule)
    g_fade_handle = rtos_task_create(&g_fade_tcb, g_fade_stack, 256,
                                      fade_task, NULL, "fade", 2);

    // Create pause task (lower priority — only needs to run every 10 s)
    rtos_task_create(&g_pause_tcb, g_pause_stack, 256,
                     pause_task, NULL, "pause", 3);

    rtos_start();   // never returns
    return 0;
}

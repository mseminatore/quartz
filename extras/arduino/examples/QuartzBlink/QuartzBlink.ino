/*
 * QuartzBlink — Minimal two-task Arduino demo for QuartzRTOS.
 *
 * Task 1 (blink_task):   Toggles LED_BUILTIN every 500 ms using
 *                        rtos_task_delay_until() for drift-free timing.
 * Task 2 (monitor_task): Watches the LED toggle and prints the measured
 *                        period over Serial every 10 toggles.
 *
 * IMPORTANT — Timer1 conflict
 * ---------------------------
 * QuartzRTOS uses Timer1 (CTC mode) for its tick interrupt on AVR.
 * You MUST NOT use the following in the same sketch:
 *   - tone() / noTone()
 *   - The Servo library
 *   - The TimerOne library
 *   - Any other library that configures Timer1
 *
 * Installation
 * ------------
 * Sketch → Include Library → Add .ZIP Library → select extras/arduino/
 * (or install via the Arduino Library Manager once published)
 *
 * Configuration (optional — edit before #include <rtos.h>)
 * ---------------------------------------------------------
 * #define RTOS_MAX_TASKS 6         // default: 8 for AVR
 * #define RTOS_TICK_RATE_HZ 500   // default: 1000 (1 ms tick)
 *
 * Tested on: Arduino Uno (ATmega328P)
 */

#include <rtos.h>

// ---------------------------------------------------------------------------
// Shared state between tasks (volatile since accessed from both)
// ---------------------------------------------------------------------------
static volatile rtos_tick_t g_toggle_tick  = 0;
static volatile int         g_toggle_count = 0;

// ---------------------------------------------------------------------------
// Blink task — drift-free 500 ms LED toggle
// ---------------------------------------------------------------------------

static rtos_tcb_t   blink_tcb;
static rtos_stack_t blink_stack[96];   // 96 bytes on AVR

static void blink_task(void *arg)
{
    (void)arg;
    pinMode(LED_BUILTIN, OUTPUT);

    int         state     = LOW;
    rtos_tick_t last_wake = rtos_task_tick_count();

    for (;;) {
        state = (state == LOW) ? HIGH : LOW;
        digitalWrite(LED_BUILTIN, state);
        g_toggle_tick  = rtos_task_tick_count();
        g_toggle_count++;
        rtos_task_delay_until(&last_wake, 500);   // 500 ticks = 500 ms
    }
}

// ---------------------------------------------------------------------------
// Monitor task — counts LED toggles and prints timing via Serial
// ---------------------------------------------------------------------------

static rtos_tcb_t   monitor_tcb;
static rtos_stack_t monitor_stack[128];

static void monitor_task(void *arg)
{
    (void)arg;

    // Wait for the first toggle
    while (g_toggle_count == 0)
        rtos_task_delay(1);

    int         last_count = g_toggle_count;
    rtos_tick_t last_tick  = g_toggle_tick;

    rtos_tick_t period_min = (rtos_tick_t)-1;
    rtos_tick_t period_max = 0;
    rtos_tick_t period_sum = 0;
    int         window     = 0;
    int         batch      = 1;

    for (;;) {
        rtos_task_delay(1);

        int cur_count = g_toggle_count;
        if (cur_count == last_count)
            continue;

        rtos_tick_t cur_tick = g_toggle_tick;
        rtos_tick_t period   = cur_tick - last_tick;

        if (period < period_min) period_min = period;
        if (period > period_max) period_max = period;
        period_sum += period;
        window++;

        last_count = cur_count;
        last_tick  = cur_tick;

        if (window == 10) {
            rtos_tick_t avg = period_sum / 10;
            Serial.print("[monitor] periods ");
            Serial.print((batch - 1) * 10 + 1);
            Serial.print("-");
            Serial.print(batch * 10);
            Serial.print(": min=");
            Serial.print(period_min);
            Serial.print(" max=");
            Serial.print(period_max);
            Serial.print(" avg=");
            Serial.println(avg);

            period_min = (rtos_tick_t)-1;
            period_max = 0;
            period_sum = 0;
            window     = 0;
            batch++;
        }
    }
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup()
{
    Serial.begin(9600);

    rtos_task_create(&blink_tcb,   blink_stack,   96,  blink_task,   NULL, "blink",   1);
    rtos_task_create(&monitor_tcb, monitor_stack, 128, monitor_task, NULL, "monitor", 2);

    // rtos_start() never returns; loop() will never execute.
    rtos_start();
}

void loop()
{
    // Unreachable — the RTOS scheduler takes over in setup().
}

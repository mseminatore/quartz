// Copyright 2025. All rights reserved.
//
// adc_pipeline — RTOS queue-based data pipeline demo.
//
// A sampler task reads the ADC at exactly 100 Hz (every 10 ms) using
// rtos_task_delay_until() for a drift-free sample rate.  Readings are posted
// to a queue.  A monitor task consumes the queue, maintains an 8-sample
// rolling average, and prints a summary once per second.
//
// On RP2040 / Pico W the sample reads ADC channel 4 — the built-in
// temperature sensor — so no external hardware is needed.  The conversion
// from raw counts to degrees Celsius follows the RP2040 datasheet formula.
//
// This pattern appears in virtually every sensor application:
//   - Sampler task: timing-critical, minimal work, fast queue post.
//   - Monitor task: timing-relaxed, heavier processing, can block on queue.
//   - The queue acts as an elastic buffer that absorbs rate mismatches.
//
// Build (RP2040 / Pico W):
//   export PICO_SDK_PATH=~/pico-sdk
//   cmake -B build && cmake --build build
//   # Connect USB; open serial terminal.  Temperature readings print every
//   # second.
//
// Build (host simulation):
//   cmake -B build && cmake --build build && ./build/sample_adc_pipeline
//   # Simulated sine-wave readings are printed instead of real temperature.

#include "rtos.h"
#include <stdint.h>
#include <stdio.h>
#include <math.h>   // sinf — only used in the host stub

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

#define SAMPLE_RATE_HZ       100u          // ADC samples per second
#define SAMPLE_PERIOD_TICKS  (RTOS_TICK_RATE_HZ / SAMPLE_RATE_HZ)  // 10 ticks
#define AVG_WINDOW           8u            // rolling average window size
#define PRINT_INTERVAL_TICKS RTOS_TICK_RATE_HZ  // print every 1 second

// ---------------------------------------------------------------------------
// Hardware abstraction — real ADC on RP2040, simulated on host
// ---------------------------------------------------------------------------

#ifdef __rp2040__
#   include "pico/stdlib.h"
#   include "hardware/adc.h"

static void hw_init(void)
{
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);   // channel 4 = internal temperature sensor
}

// Returns raw 12-bit ADC reading (0–4095)
static uint16_t hw_adc_read(void)
{
    return adc_read();
}

// Convert raw count to °C (RP2040 datasheet §4.9.5)
static float hw_adc_to_celsius(uint16_t raw)
{
    float voltage = raw * (3.3f / 4096.0f);
    return 27.0f - (voltage - 0.706f) / 0.001721f;
}

#else  // host simulation

static float g_sim_phase = 0.0f;

static void hw_init(void)
{
    printf("[host] ADC simulation active — generating synthetic temperature data\n");
}

// Simulate a temperature reading that drifts between 25 °C and 35 °C
static uint16_t hw_adc_read(void)
{
    g_sim_phase += 0.05f;
    float celsius = 30.0f + 5.0f * sinf(g_sim_phase);
    // Invert the RP2040 formula: raw = (27 - celsius) / 0.001721 + 0.706/3.3*4096
    float voltage = (27.0f - celsius) * 0.001721f + 0.706f;
    return (uint16_t)(voltage / 3.3f * 4096.0f);
}

static float hw_adc_to_celsius(uint16_t raw)
{
    float voltage = raw * (3.3f / 4096.0f);
    return 27.0f - (voltage - 0.706f) / 0.001721f;
}

#endif  // __rp2040__

// ---------------------------------------------------------------------------
// Queue — sampler → monitor
// ---------------------------------------------------------------------------

#define QUEUE_DEPTH  16u

static rtos_queue_t  g_queue;
static uint8_t       g_queue_buf[QUEUE_DEPTH * sizeof(uint16_t)];
static rtos_handle_t g_queue_handle;

// ---------------------------------------------------------------------------
// Sampler task — 100 Hz, drift-free
// ---------------------------------------------------------------------------

static rtos_tcb_t g_sampler_tcb;
static rtos_stack_t   g_sampler_stack[256];

static void sampler_task(void *arg)
{
    (void)arg;
    rtos_tick_t last_wake = rtos_task_tick_count();

    for (;;) {
        uint16_t raw = hw_adc_read();
        // Non-blocking send; drop on queue full (monitor is too slow)
        rtos_queue_send(g_queue_handle, &raw, RTOS_NO_WAIT);
        rtos_task_delay_until(&last_wake, SAMPLE_PERIOD_TICKS);
    }
}

// ---------------------------------------------------------------------------
// Monitor task — rolling average, prints once per second
// ---------------------------------------------------------------------------

static rtos_tcb_t g_monitor_tcb;
static rtos_stack_t   g_monitor_stack[256];

static void monitor_task(void *arg)
{
    (void)arg;

    float    window[AVG_WINDOW] = {0};
    uint32_t w_idx   = 0;
    uint32_t samples = 0;
    float    sum     = 0.0f;

    rtos_tick_t last_print = rtos_task_tick_count();

    for (;;) {
        uint16_t raw;
        // Block up to 50 ms waiting for the next sample
        if (rtos_queue_receive(g_queue_handle, &raw, SAMPLE_PERIOD_TICKS * 5) != RTOS_OK)
            continue;

        float celsius = hw_adc_to_celsius(raw);

        // Maintain rolling sum
        sum -= window[w_idx];
        window[w_idx] = celsius;
        sum += celsius;
        w_idx = (w_idx + 1u) % AVG_WINDOW;
        if (samples < AVG_WINDOW) samples++;

        // Print summary once per second
        if ((int32_t)(rtos_task_tick_count() - last_print) >= (int32_t)PRINT_INTERVAL_TICKS) {
            float avg = (samples > 0) ? (sum / (float)samples) : 0.0f;
            printf("temp raw=%4u  instant=%5.1f C  avg(8)=%5.1f C\n",
                   raw, celsius, avg);
            last_print += PRINT_INTERVAL_TICKS;
        }
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(void)
{
    hw_init();

    g_queue_handle = rtos_queue_create(&g_queue, g_queue_buf,
                                        sizeof(uint16_t), QUEUE_DEPTH);

    rtos_task_create(&g_sampler_tcb, g_sampler_stack, 256,
                     sampler_task, NULL, "sampler", 1);  // higher priority

    rtos_task_create(&g_monitor_tcb, g_monitor_stack, 256,
                     monitor_task, NULL, "monitor", 2);  // lower priority

    rtos_start();   // never returns
}

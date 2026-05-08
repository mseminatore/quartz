//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// runtime_stats — CPU usage dashboard demo.
//
// Demonstrates RTOS_ENABLE_RUNTIME_STATS=1 with rtos_task_get_runtime_stats().
//
// Three tasks with different CPU profiles run concurrently:
//
//   busy_task   — spins in a tight loop at lowest priority; consumes all
//                 leftover CPU that the higher-priority tasks do not use
//   medium_task — does a small computation then delays 100 ms; moderate load
//   stats_task  — sleeps 5 s, then wakes and prints a CPU-usage table
//
// Expected output (approximate, hardware):
//
//   Task              Runtime(ticks)  CPU%
//   ----------------  --------------  ----
//   busy                        4850   97%
//   medium                        50    1%
//   stats                          5    0%
//   idle                          95    2%
//
// The numbers are cumulative since boot.  Run for several minutes to see
// the percentages stabilise.
//
// Build (host simulation):
//   cmake -B build && cmake --build build && ./build/sample_runtime_stats
//   # Host port is cooperative; percentages reflect simulation ticks,
//   # not real wall-clock CPU time.
//
// Build (RP2040 / Pico W):
//   export PICO_SDK_PATH=~/pico-sdk
//   cmake -B build_pico && cmake --build build_pico
//   # Flash build_pico/sample_runtime_stats.uf2; open USB serial to see output.
//---------------------------------------------------------------------------

#include "rtos.h"
#include <stdio.h>
#include <stdint.h>

#ifdef __rp2040__
#   include "pico/stdlib.h"
#endif

// ---------------------------------------------------------------------------
// Busy task — tight loop at lowest priority; consumes all leftover CPU
// ---------------------------------------------------------------------------

static rtos_tcb_t   g_busy_tcb;
static rtos_stack_t g_busy_stack[256];

static volatile uint32_t g_busy_iterations = 0;

static void busy_task(void *arg)
{
    (void)arg;
    for (;;) {
        g_busy_iterations++;
#ifndef __rp2040__
        // Host port is cooperative: yield on every iteration so the tick
        // handler can deliver time to higher-priority tasks.
        // On hardware, the preemptive SysTick interrupt handles this automatically.
        rtos_task_yield();
#endif
    }
}

// ---------------------------------------------------------------------------
// Medium task — small computation, then 100 ms delay
// ---------------------------------------------------------------------------

static rtos_tcb_t   g_medium_tcb;
static rtos_stack_t g_medium_stack[256];

static void medium_task(void *arg)
{
    (void)arg;
    volatile uint32_t acc     = 0;
    rtos_tick_t       last_wake = rtos_task_tick_count();

    for (;;) {
        for (uint32_t i = 0; i < 1000u; i++)
            acc += i;
        (void)acc;
        rtos_task_delay_until(&last_wake, 100);   // 100 ms period
    }
}

// ---------------------------------------------------------------------------
// Stats task — prints CPU usage table every 5 s
// ---------------------------------------------------------------------------

static rtos_tcb_t   g_stats_tcb;
static rtos_stack_t g_stats_stack[512];   // larger: printf uses more stack

#define MAX_TASKS  RTOS_MAX_TASKS

static void stats_task(void *arg)
{
    (void)arg;
    rtos_runtime_stat_t buf[MAX_TASKS];
    rtos_tick_t         last_wake = rtos_task_tick_count();

    // Give other tasks 5 s to accumulate runtime before the first print
    rtos_task_delay_until(&last_wake, 5000);

    for (;;) {
        size_t n = rtos_task_get_runtime_stats(buf, MAX_TASKS);

        printf("\n%-16s  %14s  %4s\n", "Task", "Runtime(ticks)", "CPU%");
        printf("%-16s  %14s  %4s\n", "----------------", "--------------", "----");
        for (size_t i = 0; i < n; i++) {
            printf("%-16s  %14lu  %3u%%\n",
                   buf[i].name,
                   (unsigned long)buf[i].runtime_ticks,
                   (unsigned)buf[i].percent);
        }

        rtos_task_delay_until(&last_wake, 5000);   // print every 5 s
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

    // stats_task at highest priority so it always prints on schedule
    rtos_task_create(&g_stats_tcb, g_stats_stack, 512,
                     stats_task, NULL, "stats", 1);

    // medium_task at middle priority
    rtos_task_create(&g_medium_tcb, g_medium_stack, 256,
                     medium_task, NULL, "medium", 2);

    // busy_task at lowest worker priority — eats remaining CPU
    rtos_task_create(&g_busy_tcb, g_busy_stack, 256,
                     busy_task, NULL, "busy", 3);

    rtos_start();   // never returns
    return 0;
}

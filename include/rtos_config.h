//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Compile-time configuration for the RTOS.
// Override any of these by defining them before including rtos.h.
//---------------------------------------------------------------------------
#ifndef RTOS_CONFIG_H
#define RTOS_CONFIG_H

// Maximum number of tasks (including the idle task)
#ifndef RTOS_MAX_TASKS
#   define RTOS_MAX_TASKS           16
#endif

// Number of distinct priority levels (0 = highest)
#ifndef RTOS_MAX_PRIORITIES
#   define RTOS_MAX_PRIORITIES       8
#endif

// Tick rate in Hz (SysTick fires this many times per second)
#ifndef RTOS_TICK_RATE_HZ
#   define RTOS_TICK_RATE_HZ      1000
#endif

// Maximum number of software timers
#ifndef RTOS_MAX_TIMERS
#   define RTOS_MAX_TIMERS           8
#endif

// Maximum length of a task or timer name (including null terminator)
#ifndef RTOS_TASK_NAME_LEN
#   define RTOS_TASK_NAME_LEN       16
#endif

// Idle task stack size in RTOS_STACK_BYTES_PER_WORD units.
// On 32-bit ports (ARM, RISC-V) each unit is 4 bytes → 64 × 4 = 256 bytes.
// Reduce to 16 for AVR ATmega328P to save precious SRAM.
#ifndef RTOS_IDLE_STACK_WORDS
#   define RTOS_IDLE_STACK_WORDS    64
#endif

// Bytes per stack allocation unit.  4 on 32-bit ports, 1 on AVR 8-bit ports.
#ifndef RTOS_STACK_BYTES_PER_WORD
#   define RTOS_STACK_BYTES_PER_WORD  4
#endif

// --------------------------------------------------------------------------
// RISC-V CLINT configuration (only used by port/riscv/port.c and port_asm.S)
// --------------------------------------------------------------------------

// Base address of the Core-Local Interruptor (CLINT).
// 0x02000000 = QEMU virt machine default and SiFive FE310 / HiFive1.
#ifndef RTOS_CLINT_BASE_ADDR
#   define RTOS_CLINT_BASE_ADDR   0x02000000UL
#endif

// Frequency of the MTIME counter in Hz.
// QEMU virt machine: 10 MHz (10 000 000).
// SiFive FE310 / HiFive1: 32 768 Hz — override in your application config.
#ifndef RTOS_MTIME_HZ
#   define RTOS_MTIME_HZ          10000000UL
#endif

// --------------------------------------------------------------------------
// ESP32-S3 (Xtensa LX7) configuration
// (only used by port/xtensa_esp32s3/port.c and port_asm.S)
// --------------------------------------------------------------------------

// CPU frequency in Hz.  ESP32-S3 runs at 240 MHz by default.
#ifndef RTOS_ESP32S3_CPU_HZ
#   define RTOS_ESP32S3_CPU_HZ    240000000UL
#endif

// Base address of Timer Group 0 (TIMG0) — used for the RTOS tick.
// This is fixed on all ESP32-S3 silicon.
#ifndef RTOS_TIMG0_BASE_ADDR
#   define RTOS_TIMG0_BASE_ADDR   0x6001F000UL
#endif

// Special timeout values
#define RTOS_WAIT_FOREVER   ((uint32_t)0xFFFFFFFFUL)
#define RTOS_NO_WAIT        ((uint32_t)0UL)

// --------------------------------------------------------------------------
// Portable tick type — rtos_tick_t
// --------------------------------------------------------------------------
// On 32-bit targets (ARM, RISC-V, Xtensa) rtos_tick_t is uint32_t, which
// gives a tick counter that overflows after ~49 days at 1000 Hz.
//
// On memory-constrained 8/16-bit targets (e.g. AVR ATmega328P) set
// RTOS_TICK_TYPE_16BIT=1.  rtos_tick_t then becomes uint16_t, which
// overflows after ~65 seconds at 1000 Hz but costs half the code and data
// space for every tick-related variable and parameter.
//
// RTOS_WAIT_FOREVER and RTOS_NO_WAIT are automatically sized to match.

#ifndef RTOS_TICK_TYPE_16BIT
#   define RTOS_TICK_TYPE_16BIT  0
#endif

#include <stdint.h>

#if RTOS_TICK_TYPE_16BIT
    typedef uint16_t rtos_tick_t;
#   define RTOS_TICK_MAX        ((rtos_tick_t)0xFFFFU)
#   undef  RTOS_WAIT_FOREVER
#   undef  RTOS_NO_WAIT
#   define RTOS_WAIT_FOREVER    ((rtos_tick_t)0xFFFFU)
#   define RTOS_NO_WAIT         ((rtos_tick_t)0U)
#else
    typedef uint32_t rtos_tick_t;
#   define RTOS_TICK_MAX        ((rtos_tick_t)0xFFFFFFFFUL)
#   undef  RTOS_WAIT_FOREVER
#   undef  RTOS_NO_WAIT
#   define RTOS_WAIT_FOREVER    ((rtos_tick_t)0xFFFFFFFFUL)
#   define RTOS_NO_WAIT         ((rtos_tick_t)0UL)
#endif

// Return codes
#define RTOS_OK     0
#define RTOS_ERR   -1
#define RTOS_TIMEOUT -2

// --------------------------------------------------------------------------
// Debug and instrumentation options
// --------------------------------------------------------------------------

// Stack overflow detection: fill the bottom 4 words of each task stack with a
// sentinel value and check the first sentinel word each tick.
// Call rtos_stack_overflow_hook() (weak, user-overridable) on detection.
// Defaults to 0 (off) — useful during development but unnecessary in production.
#ifndef RTOS_STACK_OVERFLOW_CHECK
#   define RTOS_STACK_OVERFLOW_CHECK  1
#endif

// Stack high-water mark: fill the *entire* task stack with a pattern on
// creation so that rtos_task_stack_high_water_mark() can report peak usage.
// Costs additional time at task creation. Off by default.
#ifndef RTOS_STACK_WATERMARK
#   define RTOS_STACK_WATERMARK  0
#endif

// Trace hook system: define to 1 to enable RTOS_TRACE_* callsites in the
// kernel. When 0, all macros expand to ((void)0) with zero overhead.
// Define the rtos_trace_* functions in your application to receive events.
#ifndef RTOS_ENABLE_TRACE
#   define RTOS_ENABLE_TRACE  0
#endif

// Runtime CPU statistics: adds a runtime_ticks counter to each TCB and
// provides rtos_task_get_runtime_stats(). Off by default.
#ifndef RTOS_ENABLE_RUNTIME_STATS
#   define RTOS_ENABLE_RUNTIME_STATS  0
#endif

// Idle hook: define to the name of a void fn(void) that the idle task will
// call on every iteration. Typical uses: watchdog kick, heartbeat LED.
// Example:  #define RTOS_IDLE_HOOK_FUNCTION  my_idle_hook
// (leave undefined for no hook)

// Tickless idle: when 1, the idle task computes the time until the next
// task wakeup and calls port_suppress_ticks() to sleep for that duration.
// Requires port_suppress_ticks() to be implemented for your architecture.
#ifndef RTOS_TICKLESS_IDLE
#   define RTOS_TICKLESS_IDLE  0
#endif

// --------------------------------------------------------------------------
// Multi-core (AMP) options
// --------------------------------------------------------------------------

// Number of CPU cores. 1 = single core (default).
// Set to 2 for dual-core targets (RP2040, ESP32-S3) to enable per-core
// scheduler state and spinlock-backed critical sections.
#ifndef RTOS_NUM_CORES
#   define RTOS_NUM_CORES  1
#endif

// --------------------------------------------------------------------------
// Feature enable/disable options
// --------------------------------------------------------------------------
// Each option defaults to 1 (enabled) to preserve full backward compatibility.
// Set to 0 to omit the feature and reduce flash/RAM footprint.

// Enable rtos_task_delete(). Setting to 0 saves ~488 bytes of flash.
// Most production designs create tasks at startup and never delete them.
#ifndef RTOS_ENABLE_TASK_DELETE
#   define RTOS_ENABLE_TASK_DELETE  1
#endif

// Enable rtos_task_suspend() and rtos_task_resume(). Setting to 0 saves ~276 bytes.
#ifndef RTOS_ENABLE_TASK_SUSPEND
#   define RTOS_ENABLE_TASK_SUSPEND  1
#endif

// Enable task notifications (rtos_task_notify, rtos_task_notify_wait, etc.).
// Setting to 0 saves ~592 bytes of flash and removes notif_pending from the TCB.
#ifndef RTOS_ENABLE_TASK_NOTIFY
#   define RTOS_ENABLE_TASK_NOTIFY  1
#endif

// Enable software timers (rtos_timer_*). Setting to 0 compiles out timer.c
// entirely, saving ~816 bytes of flash and 8 bytes of BSS.
#ifndef RTOS_ENABLE_SOFTWARE_TIMERS
#   define RTOS_ENABLE_SOFTWARE_TIMERS  1
#endif

// Enable priority inheritance in rtos_mutex_lock/unlock. Setting to 0 saves
// ~70 bytes and removes the base_priority field from the TCB (1 byte per task).
// Only disable if all mutex users have the same priority (no inversion risk).
#ifndef RTOS_ENABLE_PRIORITY_INHERITANCE
#   define RTOS_ENABLE_PRIORITY_INHERITANCE  1
#endif

#endif // RTOS_CONFIG_H

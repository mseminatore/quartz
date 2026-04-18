// Copyright 2025. All rights reserved.
// Compile-time configuration for the RTOS.
// Override any of these by defining them before including rtos.h.
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

// Special timeout values
#define RTOS_WAIT_FOREVER   ((uint32_t)0xFFFFFFFFUL)
#define RTOS_NO_WAIT        ((uint32_t)0UL)

// Return codes
#define RTOS_OK     0
#define RTOS_ERR   -1
#define RTOS_TIMEOUT -2

#endif // RTOS_CONFIG_H

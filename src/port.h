//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Port interface — implemented per architecture in port/<arch>/port.c and port_asm.S.
//---------------------------------------------------------------------------
#ifndef RTOS_PORT_H
#define RTOS_PORT_H

#include <stdint.h>
#include <stddef.h>
#include "../include/rtos_config.h"

// Initialise the hardware tick timer and any port-specific state.
// Called once by vRTOSStart() before launching the first task.
void port_init(uint32_t tick_rate_hz);

// Initialise the stack frame for a new task so that when the context
// switcher restores it, execution begins at func(arg).
// stack_top must point one byte past the end of the stack buffer.
// Returns the initial stack pointer value to store in tcb->sp.
void *port_init_stack(void     *stack_top,
                      void    (*func)(void *),
                      void     *arg);

// Enter / exit a critical section (disable / re-enable interrupts).
void port_enter_critical(void);
void port_exit_critical(void);

// Request a context switch at the next safe opportunity (e.g. trigger PendSV).
void port_request_reschedule(void);

// Hand execution to the first task. g_current must be set before calling.
// This function never returns.
void port_start_first_task(void);

// Monotonic 32-bit microsecond timestamp.  Used by the trace recorder; may
// be called from any context (task, ISR).  Resolution and accuracy depend
// on the port's hardware: DWT CYCCNT on M3+, hardware µs counter on RP2040,
// CLOCK_MONOTONIC on host, tick-resolution interpolation elsewhere.
// Wraps every ~71 minutes — adequate for short captures.
uint32_t port_timestamp_us(void);

// Idle power hint: put the CPU into a low-power sleep state until the next
// interrupt arrives (WFI / sleep_cpu / equivalent).  Returns immediately on
// single-threaded simulation targets.  Must not disable the tick interrupt.
// Called from the idle task; the tick ISR will wake the CPU on the next tick.
void port_cpu_idle(void);

// Tickless sleep: re-program the tick timer to fire after at most max_ticks
// ticks, execute the CPU idle instruction, then return the number of ticks
// actually elapsed (may be less than max_ticks if another interrupt fired).
// Implement this only when RTOS_TICKLESS_IDLE=1 is desired for your port.
// The default stub (in task.c) returns 0 (no ticks suppressed).
rtos_tick_t port_suppress_ticks(rtos_tick_t max_ticks);

// Return the current CPU core index (0-based).  On single-core targets,
// always returns 0.  On dual-core targets (RP2040, ESP32-S3) this reads
// a hardware register (SIO CPUID / PRID).
uint8_t port_core_id(void);

// Called by the port's tick ISR — advances the tick count, unblocks delayed
// tasks, and fires software timers.
void rtos_tick_handler(void);

// Pick the highest-priority ready task and switch g_current to it.
// If the current task was preempted (still RUNNING), it is returned to the
// ready list first.  Called from PendSV (ARM) or the timer ISR (AVR).
void rtos_context_switch(void);

#endif // RTOS_PORT_H

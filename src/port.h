//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Port interface — implemented per architecture in port/<arch>/port.c and port_asm.S.
//---------------------------------------------------------------------------
#ifndef RTOS_PORT_H
#define RTOS_PORT_H

#include <stdint.h>
#include <stddef.h>

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

// Called by the port's tick ISR — advances the tick count, unblocks delayed
// tasks, and fires software timers.
void rtos_tick_handler(void);

// Pick the highest-priority ready task and switch g_current to it.
// If the current task was preempted (still RUNNING), it is returned to the
// ready list first.  Called from PendSV (ARM) or the timer ISR (AVR).
void rtos_context_switch(void);

#endif // RTOS_PORT_H

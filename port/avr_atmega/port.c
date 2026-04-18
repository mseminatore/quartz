// Copyright 2025. All rights reserved.
// AVR ATmega (ATmega328P) port: Timer1 CTC tick, critical sections,
// stack init, and context switch entirely inside the Timer1 COMPA ISR.
//
// Stack frame layout (low address = SP, high address = initial SP - 1):
//
//   SP+0  r31          (last pushed by ISR)
//   SP+1  r30
//   SP+2  r29
//   SP+3  r28
//   SP+4  r27
//   SP+5  r26
//   SP+6  r25          = arg_high  (GCC ABI: void* arg in R25:R24)
//   SP+7  r24          = arg_low
//   SP+8  r23
//   ...
//   SP+30 r1  = 0      (zero register — GCC ABI)
//   SP+31 SREG = 0     (I bit set by first RETI)
//   SP+32 r0  = 0
//   SP+33 PC_low       (function word-address low byte)
//   SP+34 PC_high      (function word-address high byte)
//
// Total: 35 bytes consumed from the top of the stack buffer.
// port_init_stack returns (void *)(stack_top_byte - 36), i.e., SP after
// the final PUSH which leaves SP pointing one slot below r31.
//
// Hardware note (ATmega328P datasheet §7.7):
//   CALL/interrupt pushes PC low byte first, then high byte.
//   Function pointers in avr-gcc are word addresses (byte_addr / 2).

#include <stdint.h>
#include <stddef.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include "../../src/port.h"

// ---------------------------------------------------------------------------
// CPU clock — override with -DF_CPU=... on the compiler command line
// ---------------------------------------------------------------------------
#ifndef F_CPU
#   define F_CPU 16000000UL
#endif

// ---------------------------------------------------------------------------
// Critical sections
// ---------------------------------------------------------------------------

void port_enter_critical(void)
{
    cli();
}

void port_exit_critical(void)
{
    sei();
}

// ---------------------------------------------------------------------------
// port_request_reschedule — no-op on AVR.
// Context switching happens directly inside the Timer1 COMPA ISR; there is
// no deferred PendSV equivalent on AVR.
// ---------------------------------------------------------------------------

void port_request_reschedule(void)
{
    // Intentionally empty.
}

// ---------------------------------------------------------------------------
// port_init — configure Timer1 in CTC mode to fire at RTOS_TICK_RATE_HZ.
//
// With F_CPU = 16 MHz and a 1 ms tick:
//   prescaler = 64, OCR1A = (16000000 / 64 / 1000) - 1 = 249
// ---------------------------------------------------------------------------

void port_init(uint32_t tick_rate_hz)
{
    // Choose prescaler: try /8, /64, /256, /1024 for the given F_CPU.
    // We want OCR1A = (F_CPU / prescaler / tick_rate_hz) - 1 to fit in 16 bits.
    uint32_t ocr;
    uint8_t  cs_bits;

    ocr = (F_CPU / 8UL / tick_rate_hz) - 1UL;
    if (ocr <= 0xFFFFUL) {
        cs_bits = (1 << CS11);                       // prescaler /8
    } else {
        ocr = (F_CPU / 64UL / tick_rate_hz) - 1UL;
        if (ocr <= 0xFFFFUL) {
            cs_bits = (1 << CS11) | (1 << CS10);    // prescaler /64
        } else {
            ocr = (F_CPU / 256UL / tick_rate_hz) - 1UL;
            if (ocr <= 0xFFFFUL) {
                cs_bits = (1 << CS12);               // prescaler /256
            } else {
                ocr = (F_CPU / 1024UL / tick_rate_hz) - 1UL;
                cs_bits = (1 << CS12) | (1 << CS10);// prescaler /1024
            }
        }
    }

    TCCR1A = 0;                                      // Normal port operation
    TCCR1B = (1 << WGM12) | cs_bits;                // CTC mode, chosen prescaler
    OCR1A  = (uint16_t)ocr;
    TIMSK1 = (1 << OCIE1A);                         // enable COMPA interrupt
    TCNT1  = 0;
}

// ---------------------------------------------------------------------------
// port_init_stack — build the initial AVR context frame on the task's stack.
//
// stack_top must point one byte past the end of the stack buffer.
// On AVR a "word" is one byte (RTOS_STACK_BYTES_PER_WORD = 1), so
// stack_top = (uint8_t *)stack_array + stack_words.
// ---------------------------------------------------------------------------

void *port_init_stack(void     *stack_top,
                      void    (*func)(void *),
                      void     *arg)
{
    uint8_t *sp = (uint8_t *)stack_top - 1;  // initial_SP = last valid byte

    // Hardware saves PC as: [initial_SP] = PC_low, [initial_SP-1] = PC_high.
    // avr-gcc function pointers are word addresses (byte_address / 2).
    uint16_t pc = (uint16_t)(uintptr_t)func;
    *sp-- = (uint8_t)(pc & 0xFFU);           // PC_low  at initial_SP
    *sp-- = (uint8_t)(pc >> 8);              // PC_high at initial_SP - 1

    // ISR saves r0 first (actual value), then SREG (via r0), then r1-r31.
    *sp-- = 0x00U;                           // r0 = 0
    *sp-- = 0x00U;                           // SREG = 0  (RETI sets I bit)
    *sp-- = 0x00U;                           // r1  = 0 (zero register, GCC ABI)
    *sp-- = 0x00U;                           // r2
    *sp-- = 0x00U;                           // r3
    *sp-- = 0x00U;                           // r4
    *sp-- = 0x00U;                           // r5
    *sp-- = 0x00U;                           // r6
    *sp-- = 0x00U;                           // r7
    *sp-- = 0x00U;                           // r8
    *sp-- = 0x00U;                           // r9
    *sp-- = 0x00U;                           // r10
    *sp-- = 0x00U;                           // r11
    *sp-- = 0x00U;                           // r12
    *sp-- = 0x00U;                           // r13
    *sp-- = 0x00U;                           // r14
    *sp-- = 0x00U;                           // r15
    *sp-- = 0x00U;                           // r16
    *sp-- = 0x00U;                           // r17
    *sp-- = 0x00U;                           // r18
    *sp-- = 0x00U;                           // r19
    *sp-- = 0x00U;                           // r20
    *sp-- = 0x00U;                           // r21
    *sp-- = 0x00U;                           // r22
    *sp-- = 0x00U;                           // r23
    // r24:r25 = first function argument (GCC ABI: void* in R25:R24, R24=low)
    uint16_t a = (uint16_t)(uintptr_t)arg;
    *sp-- = (uint8_t)(a & 0xFFU);            // r24 = arg_low
    *sp-- = (uint8_t)(a >> 8);              // r25 = arg_high
    *sp-- = 0x00U;                           // r26
    *sp-- = 0x00U;                           // r27
    *sp-- = 0x00U;                           // r28
    *sp-- = 0x00U;                           // r29
    *sp-- = 0x00U;                           // r30
    *sp-- = 0x00U;                           // r31

    // sp now points one below r31 — this is the TCB->sp value (SP after
    // the final PUSH; POP will increment before loading, reading r31 first).
    return (void *)sp;
}

// ---------------------------------------------------------------------------
// port_start_first_task — load the first task's context and begin execution.
//
// Loads g_current->sp (set by rtos_scheduler_start before this is called),
// restores all saved registers, and executes RETI which jumps to the task
// function and enables interrupts.
//
// This function never returns.
// ---------------------------------------------------------------------------

__attribute__((naked))
void port_start_first_task(void)
{
    asm volatile (
        // Call rtos_current_tcb_ptr() — returns &g_current in R25:R24.
        // C ABI is satisfied: r1 = 0 (zero register, never changed between
        // reset and here), stack is valid (this is a normal C function call
        // from vRTOSStart before the scheduler takes over).
        "rcall rtos_current_tcb_ptr     \n"  // r25:r24 = &g_current
        "movw  r30, r24                 \n"  // Z = &g_current
        "ld    r26, Z+                  \n"  // r26 = g_current (low byte)
        "ld    r27, Z                   \n"  // r27 = g_current (high byte)
        // X = g_current (TCB *).  TCB->sp is at offset 0 (2 bytes).
        "ld    r28, X+                  \n"  // r28 = sp_low
        "ld    r29, X                   \n"  // r29 = sp_high
        // Set the hardware stack pointer to the first task's saved SP.
        // Must write SPH before SPL (datasheet requirement when lowering SP).
        "out   __SP_H__, r29            \n"
        "out   __SP_L__, r28            \n"
        // Restore all registers from the initial stack frame.
        "pop   r31                      \n"
        "pop   r30                      \n"
        "pop   r29                      \n"
        "pop   r28                      \n"
        "pop   r27                      \n"
        "pop   r26                      \n"
        "pop   r25                      \n"
        "pop   r24                      \n"
        "pop   r23                      \n"
        "pop   r22                      \n"
        "pop   r21                      \n"
        "pop   r20                      \n"
        "pop   r19                      \n"
        "pop   r18                      \n"
        "pop   r17                      \n"
        "pop   r16                      \n"
        "pop   r15                      \n"
        "pop   r14                      \n"
        "pop   r13                      \n"
        "pop   r12                      \n"
        "pop   r11                      \n"
        "pop   r10                      \n"
        "pop   r9                       \n"
        "pop   r8                       \n"
        "pop   r7                       \n"
        "pop   r6                       \n"
        "pop   r5                       \n"
        "pop   r4                       \n"
        "pop   r3                       \n"
        "pop   r2                       \n"
        "pop   r1                       \n"
        "pop   r0                       \n"  // r0 = saved SREG
        "out   __SREG__, r0             \n"  // restore SREG
        "pop   r0                       \n"  // restore actual r0
        // RETI: pops PC (jumps to task function) and sets the I bit,
        // enabling interrupts for the first time in this task.
        "reti                           \n"
        ::: "memory"
    );
}

// ---------------------------------------------------------------------------
// Timer1 COMPA ISR — tick + context switch.
//
// ISR_NAKED: avr-libc generates the vector table entry but adds NO prologue
// or epilogue.  We save and restore every register ourselves.
//
// Sequence:
//  1. Push all 32 registers and SREG onto the current task's stack.
//  2. Clear r1 (GCC zero-register ABI) so C helpers can be called safely.
//  3. Read SP and store into g_current->sp.
//  4. Call rtos_tick_handler() — advances tick count, unblocks delayed tasks,
//     fires software timers, and calls port_request_reschedule() (no-op here).
//  5. Call rtos_context_switch() — puts the preempted task back on the ready
//     list and switches g_current to the highest-priority ready task.
//  6. Load new g_current->sp and write it to the hardware SP.
//  7. Restore all registers from the new task's stack.
//  8. RETI — pops PC and re-enables interrupts.
// ---------------------------------------------------------------------------

ISR(TIMER1_COMPA_vect, ISR_NAKED)
{
    asm volatile (
        // ----------------------------------------------------------------
        // 1. Save context of the interrupted task.
        // Push order matches the restore order in port_start_first_task
        // and the frame built by port_init_stack.
        // ----------------------------------------------------------------
        "push  r0                       \n"  // save r0 (actual value)
        "in    r0, __SREG__             \n"  // r0 = SREG
        "push  r0                       \n"  // save SREG
        "push  r1                       \n"
        "clr   r1                       \n"  // r1 = 0  (GCC ABI: zero reg)
        "push  r2                       \n"
        "push  r3                       \n"
        "push  r4                       \n"
        "push  r5                       \n"
        "push  r6                       \n"
        "push  r7                       \n"
        "push  r8                       \n"
        "push  r9                       \n"
        "push  r10                      \n"
        "push  r11                      \n"
        "push  r12                      \n"
        "push  r13                      \n"
        "push  r14                      \n"
        "push  r15                      \n"
        "push  r16                      \n"
        "push  r17                      \n"
        "push  r18                      \n"
        "push  r19                      \n"
        "push  r20                      \n"
        "push  r21                      \n"
        "push  r22                      \n"
        "push  r23                      \n"
        "push  r24                      \n"
        "push  r25                      \n"
        "push  r26                      \n"
        "push  r27                      \n"
        "push  r28                      \n"
        "push  r29                      \n"
        "push  r30                      \n"
        "push  r31                      \n"

        // ----------------------------------------------------------------
        // 2. Save current SP into g_current->sp.
        //    Read SP into r28:r29 (Y register; callee-saved, so rcall won't
        //    clobber it after we set it below).
        //    Then call rtos_current_tcb_ptr() to get &g_current.
        // ----------------------------------------------------------------
        "in    r28, __SP_L__            \n"  // r28 = SPL (SP after all pushes)
        "in    r29, __SP_H__            \n"  // r29 = SPH

        "rcall rtos_current_tcb_ptr     \n"  // r25:r24 = &g_current
        "movw  r30, r24                 \n"  // Z = &g_current
        "ld    r26, Z+                  \n"  // r26 = g_current (low byte)
        "ld    r27, Z                   \n"  // r27 = g_current (high byte)
        // X = g_current (TCB *), TCB->sp at offset 0 (2 bytes).
        "st    X+, r28                  \n"  // TCB->sp_low  = SPL
        "st    X,  r29                  \n"  // TCB->sp_high = SPH

        // ----------------------------------------------------------------
        // 3. Advance the tick and update g_current.
        // ----------------------------------------------------------------
        "rcall rtos_tick_handler        \n"  // tick, unblock tasks, timers
        "rcall rtos_context_switch      \n"  // pick next task, update g_current

        // ----------------------------------------------------------------
        // 4. Load new task's SP.
        // ----------------------------------------------------------------
        "rcall rtos_current_tcb_ptr     \n"  // r25:r24 = &g_current
        "movw  r30, r24                 \n"  // Z = &g_current
        "ld    r26, Z+                  \n"  // r26 = new g_current (low byte)
        "ld    r27, Z                   \n"  // r27 = new g_current (high byte)
        "ld    r28, X+                  \n"  // r28 = new sp_low
        "ld    r29, X                   \n"  // r29 = new sp_high
        // Write SPH first (required by datasheet when changing stack pointer).
        "out   __SP_H__, r29            \n"
        "out   __SP_L__, r28            \n"

        // ----------------------------------------------------------------
        // 5. Restore new task's context and return to it.
        // ----------------------------------------------------------------
        "pop   r31                      \n"
        "pop   r30                      \n"
        "pop   r29                      \n"
        "pop   r28                      \n"
        "pop   r27                      \n"
        "pop   r26                      \n"
        "pop   r25                      \n"
        "pop   r24                      \n"
        "pop   r23                      \n"
        "pop   r22                      \n"
        "pop   r21                      \n"
        "pop   r20                      \n"
        "pop   r19                      \n"
        "pop   r18                      \n"
        "pop   r17                      \n"
        "pop   r16                      \n"
        "pop   r15                      \n"
        "pop   r14                      \n"
        "pop   r13                      \n"
        "pop   r12                      \n"
        "pop   r11                      \n"
        "pop   r10                      \n"
        "pop   r9                       \n"
        "pop   r8                       \n"
        "pop   r7                       \n"
        "pop   r6                       \n"
        "pop   r5                       \n"
        "pop   r4                       \n"
        "pop   r3                       \n"
        "pop   r2                       \n"
        "pop   r1                       \n"
        "pop   r0                       \n"  // r0 = saved SREG
        "out   __SREG__, r0             \n"  // restore SREG
        "pop   r0                       \n"  // restore actual r0
        "reti                           \n"
        ::: "memory"
    );
}

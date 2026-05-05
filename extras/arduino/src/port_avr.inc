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
#include "port.h"

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
// port_request_reschedule — trigger a software context switch on AVR.
//
// Implemented by arming Timer1 compare-match B (OCR1B) to fire one timer
// tick from "now". Because Timer1 is already running for the periodic tick,
// we get an interrupt as soon as TCNT1 == OCR1B without disturbing the
// COMPA periodic tick. The TIMER1_COMPB_vect ISR below performs only a
// context switch (no tick advance).
//
// Safe to call from task context with interrupts enabled. A no-op is
// preserved for nested calls (the OCIE1B bit set means a switch is already
// pending). When called from within an ISR (e.g. rtos_semaphore_give_from_isr)
// the request is similarly latched and serviced as soon as that ISR returns.
// ---------------------------------------------------------------------------

void port_request_reschedule(void)
{
    uint8_t sreg = SREG;
    cli();
    OCR1B = TCNT1 + 1;          // fire on the very next timer tick
    TIFR1  = (1 << OCF1B);      // clear any stale match flag
    TIMSK1 |= (1 << OCIE1B);    // enable the software-yield interrupt
    SREG = sreg;
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
// Monotonic 32-bit microsecond timestamp — combines tick count with TCNT1
// for sub-tick resolution.  Reads must be done with interrupts disabled
// because TCNT1 is 16-bit and the tick ISR may fire mid-read.
// ---------------------------------------------------------------------------
#include "rtos_task.h"

uint32_t port_timestamp_us(void)
{
    uint32_t ticks;
    uint16_t cnt;
    uint16_t top;
    uint8_t  sreg = SREG;
    cli();
    ticks = (uint32_t)rtos_task_tick_count();
    cnt   = TCNT1;
    top   = OCR1A;
    SREG  = sreg;

    uint32_t us_per_tick = 1000000UL / RTOS_TICK_RATE_HZ;
    uint32_t sub = top ? ((uint32_t)cnt * us_per_tick) / ((uint32_t)top + 1u) : 0u;
    return ticks * us_per_tick + sub;
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
// port_cpu_idle / port_core_id
//
// AVR sleep requires the application to configure the sleep mode (SM bits in
// SMCR) and set SE before calling sleep_cpu().  Because sleep configuration
// is board-specific, we rely on the weak no-op stubs in task.c by default.
// Applications that want WFI-style idle should override port_cpu_idle() in
// their own source file:
//
//   #include <avr/sleep.h>
//   void port_cpu_idle(void) { sleep_mode(); }
//
// port_core_id always returns 0 (AVR is single-core; inherited from weak stub).
// ---------------------------------------------------------------------------

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

// port_cpu_idle and port_core_id are provided by the weak stubs in task.c.
// AVR sleep requires SM bits pre-configured by the application; the weak
// no-op default is safe here since the tick ISR always wakes the CPU.

// ---------------------------------------------------------------------------
// Save/restore macros shared by the tick ISR (TIMER1_COMPA_vect) and the
// software-yield ISR (TIMER1_COMPB_vect). Both perform the same context
// save and restore around different middle work.
// ---------------------------------------------------------------------------

#define AVR_SAVE_CONTEXT()                              \
        "push  r0                       \n"             \
        "in    r0, __SREG__             \n"             \
        "push  r0                       \n"             \
        "push  r1                       \n"             \
        "clr   r1                       \n"             \
        "push  r2                       \n"             \
        "push  r3                       \n"             \
        "push  r4                       \n"             \
        "push  r5                       \n"             \
        "push  r6                       \n"             \
        "push  r7                       \n"             \
        "push  r8                       \n"             \
        "push  r9                       \n"             \
        "push  r10                      \n"             \
        "push  r11                      \n"             \
        "push  r12                      \n"             \
        "push  r13                      \n"             \
        "push  r14                      \n"             \
        "push  r15                      \n"             \
        "push  r16                      \n"             \
        "push  r17                      \n"             \
        "push  r18                      \n"             \
        "push  r19                      \n"             \
        "push  r20                      \n"             \
        "push  r21                      \n"             \
        "push  r22                      \n"             \
        "push  r23                      \n"             \
        "push  r24                      \n"             \
        "push  r25                      \n"             \
        "push  r26                      \n"             \
        "push  r27                      \n"             \
        "push  r28                      \n"             \
        "push  r29                      \n"             \
        "push  r30                      \n"             \
        "push  r31                      \n"             \
        "in    r28, __SP_L__            \n"             \
        "in    r29, __SP_H__            \n"             \
        "rcall rtos_current_tcb_ptr     \n"             \
        "movw  r30, r24                 \n"             \
        "ld    r26, Z+                  \n"             \
        "ld    r27, Z                   \n"             \
        "st    X+, r28                  \n"             \
        "st    X,  r29                  \n"

#define AVR_RESTORE_CONTEXT()                           \
        "rcall rtos_current_tcb_ptr     \n"             \
        "movw  r30, r24                 \n"             \
        "ld    r26, Z+                  \n"             \
        "ld    r27, Z                   \n"             \
        "ld    r28, X+                  \n"             \
        "ld    r29, X                   \n"             \
        "out   __SP_H__, r29            \n"             \
        "out   __SP_L__, r28            \n"             \
        "pop   r31                      \n"             \
        "pop   r30                      \n"             \
        "pop   r29                      \n"             \
        "pop   r28                      \n"             \
        "pop   r27                      \n"             \
        "pop   r26                      \n"             \
        "pop   r25                      \n"             \
        "pop   r24                      \n"             \
        "pop   r23                      \n"             \
        "pop   r22                      \n"             \
        "pop   r21                      \n"             \
        "pop   r20                      \n"             \
        "pop   r19                      \n"             \
        "pop   r18                      \n"             \
        "pop   r17                      \n"             \
        "pop   r16                      \n"             \
        "pop   r15                      \n"             \
        "pop   r14                      \n"             \
        "pop   r13                      \n"             \
        "pop   r12                      \n"             \
        "pop   r11                      \n"             \
        "pop   r10                      \n"             \
        "pop   r9                       \n"             \
        "pop   r8                       \n"             \
        "pop   r7                       \n"             \
        "pop   r6                       \n"             \
        "pop   r5                       \n"             \
        "pop   r4                       \n"             \
        "pop   r3                       \n"             \
        "pop   r2                       \n"             \
        "pop   r1                       \n"             \
        "pop   r0                       \n"             \
        "out   __SREG__, r0             \n"             \
        "pop   r0                       \n"             \
        "reti                           \n"

ISR(TIMER1_COMPA_vect, ISR_NAKED)
{
    asm volatile (
        AVR_SAVE_CONTEXT()
        "rcall rtos_tick_handler        \n"
        "rcall rtos_context_switch      \n"
        AVR_RESTORE_CONTEXT()
        ::: "memory"
    );
}

// ---------------------------------------------------------------------------
// Timer1 COMPB ISR — software-triggered context switch (no tick).
//
// Armed by port_request_reschedule(); we disable OCIE1B before context-switch
// so the same yield request only fires once. The next port_request_reschedule
// call will rearm it.
// ---------------------------------------------------------------------------
ISR(TIMER1_COMPB_vect, ISR_NAKED)
{
    asm volatile (
        AVR_SAVE_CONTEXT()
        // Disable OCIE1B so this software-yield interrupt fires only once
        // per port_request_reschedule() request.
        "lds   r24, %[timsk]            \n"
        "andi  r24, %[mask]             \n"
        "sts   %[timsk], r24            \n"
        "rcall rtos_context_switch      \n"
        AVR_RESTORE_CONTEXT()
        :: [timsk] "i" (_SFR_MEM_ADDR(TIMSK1)),
           [mask]  "i" ((uint8_t)~(1 << OCIE1B))
        : "memory"
    );
}

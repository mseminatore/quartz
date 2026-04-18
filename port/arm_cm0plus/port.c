// Copyright 2025. All rights reserved.
// ARM Cortex-M0+ port: SysTick init, critical sections, PendSV trigger.
#include <stdint.h>
#include "../../src/port.h"

// ---------------------------------------------------------------------------
// Cortex-M0+ System Control Space registers
// ---------------------------------------------------------------------------

#define SYST_CSR    (*((volatile uint32_t *)0xE000E010))  // SysTick Control & Status
#define SYST_RVR    (*((volatile uint32_t *)0xE000E014))  // SysTick Reload Value
#define SYST_CVR    (*((volatile uint32_t *)0xE000E018))  // SysTick Current Value

#define ICSR        (*((volatile uint32_t *)0xE000ED04))  // Interrupt Control & State
#define ICSR_PENDSVSET  (1u << 28)

#define SHPR3       (*((volatile uint32_t *)0xE000ED20))  // System Handler Priority 3

// Processor clock (override for your board if needed)
#ifndef RTOS_CPU_HZ
#   define RTOS_CPU_HZ  125000000UL  // RP2040 default
#endif

// ---------------------------------------------------------------------------
// Critical section (M0+ has no BASEPRI; must disable all interrupts)
// ---------------------------------------------------------------------------

void port_enter_critical(void)
{
    __asm volatile ("cpsid i" ::: "memory");
}

void port_exit_critical(void)
{
    __asm volatile ("cpsie i" ::: "memory");
}

// ---------------------------------------------------------------------------
// Trigger a PendSV (deferred context switch)
// ---------------------------------------------------------------------------

void port_request_reschedule(void)
{
    ICSR = ICSR_PENDSVSET;
    __asm volatile ("dsb" ::: "memory");
    __asm volatile ("isb" ::: "memory");
}

// ---------------------------------------------------------------------------
// Stack initialisation
//
// Cortex-M hardware exception entry automatically pushes:
//   xPSR, PC, LR, R12, R3, R2, R1, R0   (in that order, top-of-stack first)
//
// We pre-fill this frame so that the first "return from exception" restores
// registers and jumps to func(arg).
// ---------------------------------------------------------------------------

void *port_init_stack(void     *stack_top,
                      void    (*func)(void *),
                      void     *arg)
{
    // Stack must be 8-byte aligned at exception entry; pre-decrement by 1 word
    // to align (frame is 8 words = 32 bytes).
    uint32_t *sp = (uint32_t *)stack_top;
    sp--;  // padding / alignment

    // Hardware-saved exception frame (pushed by CPU on exception entry):
    *--sp = 0x01000000u;           // xPSR: Thumb bit set
    *--sp = (uint32_t)func;        // PC   — start of task function
    *--sp = 0xFFFFFFFDu;           // LR   — EXC_RETURN: return to Thread/PSP
    *--sp = 0x00000000u;           // R12
    *--sp = 0x00000000u;           // R3
    *--sp = 0x00000000u;           // R2
    *--sp = 0x00000000u;           // R1
    *--sp = (uint32_t)arg;         // R0   — first argument to func

    // Software-saved registers (saved/restored by PendSV_Handler):
    *--sp = 0x00000000u;           // R11
    *--sp = 0x00000000u;           // R10
    *--sp = 0x00000000u;           // R9
    *--sp = 0x00000000u;           // R8
    *--sp = 0x00000000u;           // R7
    *--sp = 0x00000000u;           // R6
    *--sp = 0x00000000u;           // R5
    *--sp = 0x00000000u;           // R4

    return sp;
}

// ---------------------------------------------------------------------------
// SysTick ISR — increments the tick and triggers PendSV
// ---------------------------------------------------------------------------

void SysTick_Handler(void)
{
    extern void rtos_tick_handler(void);
    rtos_tick_handler();
}

// ---------------------------------------------------------------------------
// Port initialisation — configure SysTick and set PendSV to lowest priority
// ---------------------------------------------------------------------------

void port_init(uint32_t tick_rate_hz)
{
    uint32_t reload = (RTOS_CPU_HZ / tick_rate_hz) - 1u;

    SYST_RVR = reload;
    SYST_CVR = 0;
    // Use processor clock, enable interrupt, enable counter
    SYST_CSR = 0x07u;

    // Set PendSV to lowest priority (0xFF) so it runs after all real ISRs.
    // SHPR3 bits [23:16] = PendSV priority
    SHPR3 |= (0xFFu << 16);
}

// ---------------------------------------------------------------------------
// port_start_first_task — switch to PSP and restore the first task's context.
// Called by vRTOSStart() after the scheduler has set g_current.
// ---------------------------------------------------------------------------

__attribute__((naked))
void port_start_first_task(void)
{
    __asm volatile (
        // Get g_current->sp (offset 0 in TCB)
        "bl    rtos_current_tcb_ptr  \n"  // r0 = &g_current
        "ldr   r0, [r0]              \n"  // r0 = g_current
        "ldr   r0, [r0, #0]          \n"  // r0 = g_current->sp

        // Switch Thread mode to use PSP
        "msr   psp, r0               \n"
        "movs  r0, #2                \n"
        "msr   control, r0           \n"
        "isb                         \n"

        // Restore R4-R11 from the software-saved part of the stack
        "mrs   r0, psp               \n"
        "ldmia r0!, {r4-r7}          \n"
        "mov   r8,  r4               \n"
        "mov   r9,  r5               \n"
        "mov   r10, r6               \n"
        "mov   r11, r7               \n"
        "ldmia r0!, {r4-r7}          \n"
        "msr   psp, r0               \n"

        // EXC_RETURN: return to Thread mode, PSP, no FPU
        "ldr   r0, =0xFFFFFFFD       \n"
        "bx    r0                    \n"
        ::: "memory"
    );
}

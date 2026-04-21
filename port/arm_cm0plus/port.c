// Copyright 2025. All rights reserved.
// ARM Cortex-M0+ port: SysTick init, critical sections, PendSV trigger.
#include <stdint.h>
#include "../../src/port.h"
#include "../../include/rtos_config.h"

// ---------------------------------------------------------------------------
// Cortex-M0+ System Control Space registers
// ---------------------------------------------------------------------------

#define SYST_CSR    (*((volatile uint32_t *)0xE000E010))  // SysTick Control & Status
#define SYST_RVR    (*((volatile uint32_t *)0xE000E014))  // SysTick Reload Value
#define SYST_CVR    (*((volatile uint32_t *)0xE000E018))  // SysTick Current Value

#define ICSR        (*((volatile uint32_t *)0xE000ED04))  // Interrupt Control & State
#define ICSR_PENDSVSET  (1u << 28)
#define ICSR_PENDSVCLR  (1u << 27)

#define SHPR3       (*((volatile uint32_t *)0xE000ED20))  // System Handler Priority 3

// Processor clock (override for your board if needed)
#ifndef RTOS_CPU_HZ
#   define RTOS_CPU_HZ  125000000UL  // RP2040 default
#endif

// ---------------------------------------------------------------------------
// RP2040 SIO spinlock support (used for multi-core critical sections)
// ---------------------------------------------------------------------------

#if RTOS_NUM_CORES > 1
// SIO base address (RP2040 datasheet §2.3.1)
#define SIO_BASE            0xD0000000u
// Spinlock 0 — claim/release by reading/writing the register.
// Read returns non-zero if the lock was successfully claimed (and atomically
// marks it taken); write of any value releases the lock.
#define SIO_SPINLOCK0       (*((volatile uint32_t *)(SIO_BASE + 0x100u)))

static void spinlock_acquire(void)
{
    // Spin until we atomically claim spinlock 0
    while (SIO_SPINLOCK0 == 0u)
        __asm volatile ("nop");
    __asm volatile ("dmb" ::: "memory");
}

static void spinlock_release(void)
{
    __asm volatile ("dmb" ::: "memory");
    SIO_SPINLOCK0 = 0u;   // any write releases the lock
}
#endif  // RTOS_NUM_CORES > 1

// ---------------------------------------------------------------------------
// Critical section (M0+ has no BASEPRI; must disable all interrupts)
// For multi-core: also acquire a hardware spinlock so that core 1 is
// excluded from the critical section while core 0 holds it, and vice-versa.
// ---------------------------------------------------------------------------

void port_enter_critical(void)
{
    __asm volatile ("cpsid i" ::: "memory");
#if RTOS_NUM_CORES > 1
    spinlock_acquire();
#endif
}

void port_exit_critical(void)
{
#if RTOS_NUM_CORES > 1
    spinlock_release();
#endif
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

// ---------------------------------------------------------------------------
// port_cpu_idle — execute WFI to sleep until the next interrupt.
// The CPU wakes on SysTick (or any other interrupt), then PendSV fires the
// context switch as normal.  Must not disable the tick interrupt.
// ---------------------------------------------------------------------------

void port_cpu_idle(void)
{
    __asm volatile ("wfi" ::: "memory");
}

// ---------------------------------------------------------------------------
// port_core_id — return the current CPU core index (0 or 1).
// Reads the SIO CPUID register at 0xD0000000 offset 0x0.
// Always 0 when RTOS_NUM_CORES == 1 so the compiler can constant-fold it.
// ---------------------------------------------------------------------------

uint8_t port_core_id(void)
{
#if RTOS_NUM_CORES > 1
    return (uint8_t)(*(volatile uint32_t *)0xD0000000u);
#else
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// port_suppress_ticks — tickless idle for ARM Cortex-M0+.
//
// Reprograms SysTick to fire after at most max_ticks normal-tick periods,
// executes WFI to sleep, then measures how many ticks actually elapsed and
// restores the normal SysTick period before returning.
//
// The caller (idle task in task.c) calls rtos_tick_advance(elapsed) to credit
// those ticks to the scheduler.
//
// Preconditions: interrupts are ENABLED when this is called (called from the
// idle task body, outside critical sections).  SysTick fires even during WFI.
// ---------------------------------------------------------------------------

#if RTOS_TICKLESS_IDLE
uint32_t port_suppress_ticks(uint32_t max_ticks)
{
    if (max_ticks == 0) return 0;

    // The reload value for one normal tick
    uint32_t one_tick_reload = (RTOS_CPU_HZ / RTOS_TICK_RATE_HZ) - 1u;

    // Cap to what fits in SysTick's 24-bit counter
    uint32_t max_reload_24   = 0x00FFFFFFu;
    uint32_t max_suppressible = max_reload_24 / (one_tick_reload + 1u);
    if (max_ticks > max_suppressible)
        max_ticks = max_suppressible;

    uint32_t sleep_reload = (one_tick_reload + 1u) * max_ticks - 1u;

    // Disable SysTick, reprogram for the extended period, re-enable
    SYST_CSR = 0x00u;              // stop
    SYST_RVR = sleep_reload;
    SYST_CVR = 0;                  // clear; reload takes effect on next enable
    SYST_CSR = 0x07u;              // restart (CLKSRC=processor, TICKINT, ENABLE)

    // Sleep until any interrupt (SysTick or otherwise) fires
    __asm volatile ("wfi" ::: "memory");

    // Measure how many ticks elapsed based on remaining count
    uint32_t remaining = SYST_CVR;   // current down-counter value
    uint32_t elapsed_cycles = sleep_reload - remaining;
    uint32_t elapsed_ticks  = elapsed_cycles / (one_tick_reload + 1u);

    // If SysTick wrapped (COUNTFLAG set), we slept the full period
    if (SYST_CSR & (1u << 16))
        elapsed_ticks = max_ticks;

    // Restore normal SysTick period
    SYST_CSR = 0x00u;
    SYST_RVR = one_tick_reload;
    SYST_CVR = 0;
    SYST_CSR = 0x07u;

    // Clear any PendSV that fired during the sleep so we don't double-process
    ICSR = ICSR_PENDSVCLR;

    return elapsed_ticks;
}
#endif  // RTOS_TICKLESS_IDLE

// Copyright 2025. All rights reserved.
// ARM Cortex-M4(F) port: SysTick init, BASEPRI critical sections, PendSV trigger.
//
// Key differences from the Cortex-M0+ port:
//   - Critical sections use BASEPRI (not PRIMASK/cpsid) so that very high
//     priority ISRs (below RTOS_BASEPRI_VALUE numerically) can still preempt
//     the kernel while a critical section is held.
//   - SVC_Handler uses Thumb-2 ldmia to restore R4-R11 in one instruction.
//   - No RP2040 SIO spinlock; single-core only in this generic port.
//
// BASEPRI priority threshold:
//   RTOS_BASEPRI_VALUE defaults to 0x50 (priority 5 for 4-bit priority fields
//   shifted to the upper byte).  Any ISR that calls RTOS APIs from ISR context
//   (e.g. rtos_semaphore_give_from_isr) must be configured at a numeric priority
//   >= RTOS_BASEPRI_VALUE >> 4.  ISRs with numeric priority < (RTOS_BASEPRI_VALUE >> 4)
//   are never masked and must NOT call any RTOS API.
//
// FPU note:
//   This port saves R4-R11 only (no S16-S31).  If tasks use floating-point
//   instructions (hard-float ABI), define RTOS_CM4_FPU=1 in rtos_config.h;
//   port_init_stack and port_asm.S must then be extended to save/restore
//   S16-S31 and FPSCR (callee-saved FP registers).  S0-S15 and FPSCR are
//   saved automatically by the CPU's lazy-stacking mechanism.

#include <stdint.h>
#include "../../src/port.h"
#include "../../include/rtos_config.h"

// ---------------------------------------------------------------------------
// Cortex-M4 System Control Space registers (identical to M0+)
// ---------------------------------------------------------------------------

#define SYST_CSR    (*((volatile uint32_t *)0xE000E010))
#define SYST_RVR    (*((volatile uint32_t *)0xE000E014))
#define SYST_CVR    (*((volatile uint32_t *)0xE000E018))

#define ICSR        (*((volatile uint32_t *)0xE000ED04))
#define ICSR_PENDSVSET  (1u << 28)
#define ICSR_PENDSVCLR  (1u << 27)

#define SHPR2       (*((volatile uint32_t *)0xE000ED1C))  // System Handler Priority 2
#define SHPR3       (*((volatile uint32_t *)0xE000ED20))  // System Handler Priority 3

// Processor clock — override for your board (e.g. -DRTOS_CPU_HZ=168000000)
#ifndef RTOS_CPU_HZ
#   define RTOS_CPU_HZ  168000000UL  // STM32F4xx default (168 MHz)
#endif

// BASEPRI threshold: blocks interrupts at numeric priority >= (value >> 4).
// 0x50 = block priority >= 5 (for 4-bit priority fields).
// Raise this value to allow more ISRs to be unmasked during critical sections;
// lower it to allow fewer (more conservative).
#ifndef RTOS_BASEPRI_VALUE
#   define RTOS_BASEPRI_VALUE  0x50u
#endif

// ---------------------------------------------------------------------------
// Critical sections (BASEPRI-based — M4/M3/M7 only, not available on M0/M0+)
//
// port_enter_critical: raise BASEPRI to mask RTOS-aware interrupts.
// port_exit_critical:  restore BASEPRI to the previously saved value.
//
// Nesting is tracked: enter saves the previous BASEPRI value on a stack and
// exit restores it, so nested critical sections work correctly.
// ---------------------------------------------------------------------------

static uint32_t g_basepri_stack[8];
static uint32_t g_critical_nesting = 0;

#define RTOS_BASEPRI_STACK_DEPTH \
    (sizeof(g_basepri_stack) / sizeof(g_basepri_stack[0]))

// Optional overflow handler — application can override this to log/panic when
// critical-section nesting exceeds RTOS_BASEPRI_STACK_DEPTH (truncation point
// after which BASEPRI restore values are lost). Default: spin to halt the CPU
// rather than silently mis-restore interrupt masking.
__attribute__((weak)) void rtos_port_critical_overflow_hook(void)
{
    for (;;) { /* halt — increase RTOS_BASEPRI_STACK_DEPTH */ }
}

void port_enter_critical(void)
{
    uint32_t prev;
    __asm volatile ("mrs %0, basepri" : "=r"(prev));

    const uint32_t val = RTOS_BASEPRI_VALUE;
    __asm volatile (
        "msr basepri, %0  \n"
        "dsb              \n"
        "isb              \n"
        :: "r"(val) : "memory"
    );

    if (g_critical_nesting < RTOS_BASEPRI_STACK_DEPTH)
        g_basepri_stack[g_critical_nesting] = prev;
    else
        rtos_port_critical_overflow_hook();
    g_critical_nesting++;
}

void port_exit_critical(void)
{
    if (g_critical_nesting == 0) return;
    g_critical_nesting--;

    uint32_t restore = 0;
    if (g_critical_nesting < RTOS_BASEPRI_STACK_DEPTH)
        restore = g_basepri_stack[g_critical_nesting];

    __asm volatile (
        "msr basepri, %0  \n"
        :: "r"(restore) : "memory"
    );
}

// ---------------------------------------------------------------------------
// Trigger a PendSV (deferred context switch)
// ---------------------------------------------------------------------------

void port_request_reschedule(void)
{
    ICSR = ICSR_PENDSVSET;
    __asm volatile ("dsb \n isb \n" ::: "memory");
}

// ---------------------------------------------------------------------------
// Stack initialisation
//
// Hardware exception frame (pushed by CPU on exception entry):
//   xPSR, PC, LR, R12, R3, R2, R1, R0
//
// Software frame (saved by PendSV_Handler before the hardware frame):
//   Without FPU support  : R4, R5, R6, R7, R8, R9, R10, R11           (8 words)
//   With RTOS_CM4_FPU=1  : R4..R11 plus EXC_RETURN (LR)               (9 words)
//                          When the task has touched the FPU the CPU's lazy
//                          stacking pushes S0-S15 + FPSCR onto the hardware
//                          frame, and PendSV pushes S16-S31 onto the software
//                          frame. The saved EXC_RETURN value tells the restore
//                          path which layout is in use.
//
// Total initial stack depth: 16 words (non-FPU) or 17 words (FPU), aligned to
// 8 bytes. Initial EXC_RETURN is 0xFFFFFFFD (Thread/PSP/basic frame); the
// first FP instruction will lazily switch the task to extended frames.
// ---------------------------------------------------------------------------

void *port_init_stack(void     *stack_top,
                      void    (*func)(void *),
                      void     *arg)
{
    uint32_t *sp = (uint32_t *)stack_top;
    sp--;  // padding for 8-byte alignment (frame is 16 words = 64 bytes)

    // Hardware-saved exception frame (CPU pushes on exception entry):
    *--sp = 0x01000000u;           // xPSR: Thumb bit set, no IT state
    *--sp = (uint32_t)func;        // PC   — task entry point
    *--sp = 0xFFFFFFFDu;           // LR   — EXC_RETURN: Thread/PSP/basic frame
    *--sp = 0x00000000u;           // R12
    *--sp = 0x00000000u;           // R3
    *--sp = 0x00000000u;           // R2
    *--sp = 0x00000000u;           // R1
    *--sp = (uint32_t)arg;         // R0   — first argument to func

    // Software-saved registers (saved/restored by PendSV_Handler).
    // With FPU support we also save EXC_RETURN per-task (the bottom slot of
    // the software frame) so each task can return to either basic or extended
    // exception state independently.
#if RTOS_CM4_FPU
    *--sp = 0xFFFFFFFDu;           // EXC_RETURN — basic frame initially
#endif
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
// SysTick ISR
// ---------------------------------------------------------------------------

void SysTick_Handler(void)
{
    extern void rtos_tick_handler(void);
    rtos_tick_handler();
}

// Force port_asm.S.o into the link: PendSV_Handler is only called by
// hardware, so without this anchor the linker would silently discard it.
extern void PendSV_Handler(void);
__attribute__((used)) static void * const _port_asm_anchor = (void *)PendSV_Handler;

// ---------------------------------------------------------------------------
// port_init — configure SysTick and set exception priorities
// ---------------------------------------------------------------------------

void port_init(uint32_t tick_rate_hz)
{
    uint32_t reload = (RTOS_CPU_HZ / tick_rate_hz) - 1u;

    SYST_RVR = reload;
    SYST_CVR = 0;
    SYST_CSR = 0x07u;  // processor clock, IRQ enabled, counter enabled

    // Set PendSV to lowest priority (0xFF) so it runs after all real ISRs.
    SHPR3 |= (0xFFu << 16);

    // Set SVCall to a low priority so it can be called from Thread mode.
    SHPR2 |= (0xFFu << 24);

    // Enable DWT CYCCNT for high-resolution timestamps used by the trace
    // recorder.  Available on all ARMv7-M cores (Cortex-M3/M4/M7).
    #define DEMCR       (*((volatile uint32_t *)0xE000EDFCu))
    #define DWT_CTRL    (*((volatile uint32_t *)0xE0001000u))
    #define DWT_CYCCNT  (*((volatile uint32_t *)0xE0001004u))
    DEMCR    |= (1u << 24);   // TRCENA
    DWT_CYCCNT = 0u;
    DWT_CTRL |= 1u;           // CYCCNTENA
}

// Cycles per microsecond for DWT-based timestamps.
#define RTOS_CYCLES_PER_US  (RTOS_CPU_HZ / 1000000u)

uint32_t port_timestamp_us(void)
{
    return DWT_CYCCNT / RTOS_CYCLES_PER_US;
}

// ---------------------------------------------------------------------------
// SVC_Handler — raises exception context so that EXC_RETURN can be used.
// On M4 (Thumb-2), ldmia works with all R4-R11 in a single instruction.
// With RTOS_CM4_FPU=1 the saved software frame includes EXC_RETURN as its
// bottom slot, so the first task starts with the correct frame state.
// ---------------------------------------------------------------------------

__attribute__((naked))
void SVC_Handler(void)
{
    __asm volatile (
        // Load g_current->sp
        "bl     rtos_current_tcb_ptr    \n"  // r0 = &g_current
        "ldr    r0, [r0]                \n"  // r0 = g_current
        "ldr    r0, [r0, #0]            \n"  // r0 = g_current->sp

        // Switch Thread mode to PSP
        "msr    psp, r0                 \n"
        "movs   r0, #2                  \n"
        "msr    control, r0             \n"
        "isb                            \n"

        // Restore the software frame.
#if RTOS_CM4_FPU
        // FPU build: software frame is { R4-R11, EXC_RETURN }. Pop EXC_RETURN
        // into LR so the bx lr below honours the per-task frame state. The
        // initial frame is always basic (0xFFFFFFFD), so no S16-S31 need
        // restoring on first entry.
        "mrs    r0, psp                 \n"
        "ldmia  r0!, {r4-r11, lr}       \n"  // restore R4-R11 and EXC_RETURN
        "msr    psp, r0                 \n"
        "bx     lr                      \n"
#else
        "mrs    r0, psp                 \n"
        "ldmia  r0!, {r4-r11}           \n"  // restore R4-R11; r0 = hardware frame
        "msr    psp, r0                 \n"

        // EXC_RETURN: Thread mode, PSP, basic frame (no extended FPU frame)
        "ldr    r0, =0xFFFFFFFD         \n"
        "bx     r0                      \n"
#endif
        ::: "memory"
    );
}

// ---------------------------------------------------------------------------
// port_start_first_task — raise SVCall to enter exception context
// ---------------------------------------------------------------------------

__attribute__((naked))
void port_start_first_task(void)
{
    __asm volatile (
        "svc  0    \n"  // enter SVC_Handler
        "bx   lr   \n"  // never reached
        ::: "memory"
    );
}

// ---------------------------------------------------------------------------
// port_cpu_idle — WFI until next interrupt
// ---------------------------------------------------------------------------

void port_cpu_idle(void)
{
    __asm volatile ("wfi" ::: "memory");
}

// ---------------------------------------------------------------------------
// port_core_id — always 0 for this single-core generic port
// ---------------------------------------------------------------------------

uint8_t port_core_id(void)
{
    return 0;
}

// ---------------------------------------------------------------------------
// port_suppress_ticks — tickless idle (identical SysTick mechanism as M0+)
// ---------------------------------------------------------------------------

#if RTOS_TICKLESS_IDLE
rtos_tick_t port_suppress_ticks(rtos_tick_t max_ticks)
{
    if (max_ticks == 0) return 0;

    uint32_t one_tick_reload  = (RTOS_CPU_HZ / RTOS_TICK_RATE_HZ) - 1u;
    uint32_t max_reload_24    = 0x00FFFFFFu;
    uint32_t max_suppressible = max_reload_24 / (one_tick_reload + 1u);
    if (max_ticks > (rtos_tick_t)max_suppressible)
        max_ticks = (rtos_tick_t)max_suppressible;

    uint32_t sleep_reload = (one_tick_reload + 1u) * (uint32_t)max_ticks - 1u;

    SYST_CSR = 0x00u;
    SYST_RVR = sleep_reload;
    SYST_CVR = 0;
    SYST_CSR = 0x07u;

    __asm volatile ("wfi" ::: "memory");

    uint32_t     remaining     = SYST_CVR;
    rtos_tick_t  elapsed_ticks = (rtos_tick_t)((sleep_reload - remaining) / (one_tick_reload + 1u));

    if (SYST_CSR & (1u << 16))
        elapsed_ticks = max_ticks;

    SYST_CSR = 0x00u;
    SYST_RVR = one_tick_reload;
    SYST_CVR = 0;
    SYST_CSR = 0x07u;

    ICSR = ICSR_PENDSVCLR;

    return elapsed_ticks;
}
#endif  // RTOS_TICKLESS_IDLE

// Copyright 2025. All rights reserved.
// ARM Cortex-M4 context switch — PendSV_Handler.
//
// Unlike Cortex-M0+, the M4 (Thumb-2) instruction set supports stmdb/ldmia
// with all registers R0-R15 in a single instruction, eliminating the
// two-step dance required on M0+ for R8-R11.
//
// On exception entry the CPU has already pushed the basic hardware frame:
//   { R0, R1, R2, R3, R12, LR, PC, xPSR }  onto the task's PSP stack.
//
// PendSV_Handler:
//   1. Saves R4-R11 of the outgoing task (stmdb onto PSP).
//   2. Saves the updated SP in the TCB.
//   3. Calls rtos_context_switch() to update g_current.
//   4. Loads the new task's SP from its TCB.
//   5. Restores R4-R11 of the incoming task (ldmia from PSP).
//   6. Sets PSP to point at the hardware frame and performs EXC_RETURN.
//
// With RTOS_CM4_FPU=1 we additionally:
//   - Conditionally save S16-S31 onto the outgoing task's PSP when bit 4 of
//     EXC_RETURN is clear (extended frame in use).
//   - Save EXC_RETURN as the bottom slot of the software frame so each task
//     remembers whether it owns extended FP context.
//   - Mirror the conditional restore on the incoming task.
//   The CPU's lazy-stacking already preserves S0-S15 + FPSCR.
//
// TCB layout (must match rtos_tcb_t in rtos_task.h):
//   offset 0: sp (void *)  — the only field touched by this assembly.

// RTOS_CM4_FPU is propagated from CMakeLists.txt via target_compile_definitions
// (we deliberately do *not* #include rtos_config.h here, since the assembler
// cannot parse the C typedefs it transitively includes from <stdint.h>).

    .syntax unified
    .thumb
    .text

    .global PendSV_Handler
    .type   PendSV_Handler, %function
    .thumb_func

PendSV_Handler:
    // -----------------------------------------------------------------------
    // 1. Save context of the outgoing task onto its PSP stack.
    // -----------------------------------------------------------------------
    mrs     r0, psp             // r0 = current PSP (top of task's stack)
#if RTOS_CM4_FPU
    // If the task was using the FPU (EXC_RETURN bit 4 == 0) we must save
    // the callee-saved single-precision regs S16-S31 ourselves. S0-S15 and
    // FPSCR are already on the hardware frame thanks to lazy stacking.
    tst     lr, #0x10
    it      eq
    vstmdbeq r0!, {s16-s31}
    // Save R4-R11 plus EXC_RETURN so the incoming-task path can restore the
    // correct frame state on its bx lr.
    stmdb   r0!, {r4-r11, lr}
#else
    stmdb   r0!, {r4-r11}       // push R4-R11 (pre-decrement); r0 = new SP
#endif

    // -----------------------------------------------------------------------
    // 2. Store updated SP into g_current->sp  (offset 0 in TCB).
    //    Preserve r0 (new SP) and lr (EXC_RETURN) across BL calls using MSP.
    // -----------------------------------------------------------------------
    push    {r0, lr}                // save new SP and EXC_RETURN on MSP
    bl      rtos_current_tcb_ptr    // r0 = &g_current  (rtos_tcb_t **)
    ldr     r1, [r0]                // r1 = g_current  (rtos_tcb_t *)
    pop     {r2, lr}                // r2 = saved new SP; lr = EXC_RETURN
    str     r2, [r1, #0]            // g_current->sp = new SP

    // -----------------------------------------------------------------------
    // 3. Update g_current via rtos_context_switch().
    //    This re-queues the outgoing task (if RUNNING) and selects the
    //    highest-priority ready task as the new g_current.
    // -----------------------------------------------------------------------
    push    {lr}
    bl      rtos_context_switch     // updates g_current
    pop     {lr}

    // -----------------------------------------------------------------------
    // 4. Load the incoming task's SP.
    // -----------------------------------------------------------------------
    push    {lr}
    bl      rtos_current_tcb_ptr    // r0 = &g_current
    pop     {lr}
    ldr     r1, [r0]                // r1 = new g_current (rtos_tcb_t *)
    ldr     r0, [r1, #0]            // r0 = new task's saved SP

    // -----------------------------------------------------------------------
    // 5. Restore the incoming task's software frame.
    //    ldmia advances r0 past the saved words, leaving r0 pointing at
    //    the hardware exception frame (or the FPU extended frame).
    // -----------------------------------------------------------------------
#if RTOS_CM4_FPU
    ldmia   r0!, {r4-r11, lr}   // restore R4-R11 and per-task EXC_RETURN
    tst     lr, #0x10
    it      eq
    vldmiaeq r0!, {s16-s31}     // restore callee-saved FPU regs if extended
#else
    ldmia   r0!, {r4-r11}       // restore R4-R11; r0 = hardware frame
#endif

    // -----------------------------------------------------------------------
    // 6. Update PSP to the hardware frame and return to Thread mode.
    // -----------------------------------------------------------------------
    msr     psp, r0             // PSP now points at hardware exception frame
    bx      lr                  // EXC_RETURN in lr → Thread mode, PSP, basic/extended frame

    .size PendSV_Handler, . - PendSV_Handler

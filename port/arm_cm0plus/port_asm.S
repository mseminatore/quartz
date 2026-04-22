// Copyright 2025. All rights reserved.
// ARM Cortex-M0+ context switch — PendSV_Handler / isr_pendsv.
//
// The Pico SDK vector table uses the name isr_pendsv; CMSIS-based projects
// use PendSV_Handler.  Both are defined below so the linker finds the right
// symbol regardless of build environment.
//
// On exception entry the CPU has already pushed:
//   { xPSR, PC, LR, R12, R3, R2, R1, R0 }  onto the PSP stack.
//
// PendSV_Handler saves R4-R11 of the outgoing task, switches the PSP to the
// incoming task, then restores R4-R11 of the incoming task. The exception
// return pops the hardware frame and resumes the new task.
//
// C symbol references:
//   rtos_current_tcb_ptr()  — returns rtos_tcb_t** (address of g_current)
//   rtos_next_task()        — returns rtos_tcb_t*  (highest-priority ready task)
//   rtos_task_make_ready()  — moves a TCB onto the ready list (used by semaphore/mutex)
//
// TCB layout (must match rtos_tcb_t in rtos_task.h):
//   offset 0: sp (uint32_t *)   — the only field the assembler touches

    .syntax unified
    .thumb
    .text

// Two symbol names are defined for the PendSV handler:
//   PendSV_Handler — used by CMSIS / bare-metal vector tables
//   isr_pendsv     — used by the Pico SDK's crt0.S vector table
// Both are strong symbols pointing to the same code.

    .global PendSV_Handler
    .type   PendSV_Handler, %function
    .global isr_pendsv
    .type   isr_pendsv, %function
    .thumb_func

PendSV_Handler:
isr_pendsv:
    // -----------------------------------------------------------------------
    // 1. Save R4-R11 of the outgoing task onto its PSP stack.
    // -----------------------------------------------------------------------
    mrs     r0, psp             // r0 = current PSP (outgoing task's stack top)
    subs    r0, r0, #32         // make room for R4-R11 (8 × 4 bytes)
    stmia   r0!, {r4-r7}        // save R4-R7  (M0+ only has stmia/ldmia for low regs)
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11
    stmia   r0!, {r4-r7}        // save R8-R11 via r4-r7 (M0+ limitation)
    subs    r0, r0, #32         // restore r0 to point at bottom of saved frame

    // -----------------------------------------------------------------------
    // 2. Store updated SP into g_current->sp  (offset 0 in TCB).
    // -----------------------------------------------------------------------
    bl      rtos_current_tcb_ptr    // r0 = &g_current  (rtos_tcb_t **)
    ldr     r1, [r0]                // r1 = g_current  (rtos_tcb_t *)
    mrs     r2, psp
    subs    r2, r2, #32
    str     r2, [r1, #0]            // g_current->sp = updated PSP

    // -----------------------------------------------------------------------
    // 3. Update g_current via rtos_context_switch() and load new task's SP.
    // rtos_context_switch() puts the preempted task back on the ready list
    // (if it was still RUNNING), picks the highest-priority ready task, and
    // sets g_current to the new task.
    // -----------------------------------------------------------------------
    bl      rtos_context_switch         // updates g_current

    bl      rtos_current_tcb_ptr        // r0 = &g_current
    ldr     r1, [r0]                    // r1 = new g_current (TCB *)

    // -----------------------------------------------------------------------
    // 4. Load new task's SP and restore R4-R11.
    //
    // Stack layout: [sp+0..sp+12] = R4-R7, [sp+16..sp+28] = R8-R11
    //
    // On CM0+, ldmia only works with low regs (r0-r7), so R8-R11 must be
    // loaded into r4-r7 first, moved to high regs, then R4-R7 loaded.
    // -----------------------------------------------------------------------
    ldr     r0, [r1, #0]        // r0 = new task's saved SP (sp+0 = R4 slot)

    // Load R8-R11 first (at sp+16..sp+28) via low regs, then move to high regs.
    adds    r0, r0, #16         // r0 = sp+16 (R8 slot)
    ldmia   r0!, {r4-r7}        // r4=R8, r5=R9, r6=R10, r7=R11; r0 = sp+32
    mov     r8,  r4
    mov     r9,  r5
    mov     r10, r6
    mov     r11, r7             // r8-r11 are now correct

    // Reload R4-R7 from sp+0..sp+12.
    subs    r0, r0, #32         // r0 = sp+0
    ldmia   r0!, {r4-r7}        // r4=R4, r5=R5, r6=R6, r7=R7; r0 = sp+16

    // Advance PSP to sp+32 (the hardware exception frame).
    adds    r0, r0, #16         // r0 = sp+32
    msr     psp, r0

    // -----------------------------------------------------------------------
    // 5. Return to Thread mode using PSP.
    // -----------------------------------------------------------------------
    ldr     r0, =0xFFFFFFFD     // EXC_RETURN: Thread mode, PSP, no FPU
    bx      r0

    .size PendSV_Handler, . - PendSV_Handler
    .size isr_pendsv, . - PendSV_Handler

/* Copyright 2025. All rights reserved.
 * RISC-V (RV32IMAC) machine-mode context switch and first-task launch.
 *
 * Context frame layout (128 bytes, sp points to lowest word):
 *   [sp + 0*4]  mepc
 *   [sp + 1*4]  mstatus
 *   [sp + 2*4]  x1  (ra)
 *   [sp + 3*4]  x3  (gp)
 *   [sp + 4*4]  x4  (tp)
 *   [sp + 5*4]  x5  (t0)
 *   [sp + 6*4]  x6  (t1)
 *   [sp + 7*4]  x7  (t2)
 *   [sp + 8*4]  x8  (s0/fp)
 *   [sp + 9*4]  x9  (s1)
 *   [sp +10*4]  x10 (a0)
 *   [sp +11*4]  x11 (a1)
 *   ... (a2-a7, s2-s11, t3-t6 at offsets 12-31)
 *   [sp +31*4]  x31 (t6)
 */

    .section .text
    .option norvc           /* keep instructions 32-bit for predictable offsets */

/* -----------------------------------------------------------------------
 * port_start_first_task
 * Load g_current->sp, restore the initial context frame, mret into task.
 * ----------------------------------------------------------------------- */
    .global port_start_first_task
    .type   port_start_first_task, @function
port_start_first_task:
    la   a0, g_current          /* a0 = &g_current                        */
    lw   a0, 0(a0)              /* a0 = g_current (TCB pointer)           */
    lw   sp, 0(a0)              /* sp = g_current->sp (void* is 1st field)*/
    j    .Lrestore_and_mret

/* -----------------------------------------------------------------------
 * riscv_trap_handler  (direct mode — mtvec points here)
 * Handles machine timer interrupt (mcause = 0x80000007) and machine
 * software interrupt (mcause = 0x80000003).  All other traps fall through
 * without a context switch.
 * ----------------------------------------------------------------------- */
    .global riscv_trap_handler
    .type   riscv_trap_handler, @function
riscv_trap_handler:
    /* --- save context -------------------------------------------------- */
    addi sp, sp, -128

    /* save ra first — subsequent 'call' instructions overwrite it */
    sw   ra,   8(sp)        /* x1   [2*4]  */

    /* save remaining GPRs (skip x0=zero, x2=sp) */
    sw   gp,  12(sp)        /* x3   [3*4]  */
    sw   tp,  16(sp)        /* x4   [4*4]  */
    sw   t0,  20(sp)        /* x5   [5*4]  */
    sw   t1,  24(sp)        /* x6   [6*4]  */
    sw   t2,  28(sp)        /* x7   [7*4]  */
    sw   s0,  32(sp)        /* x8   [8*4]  */
    sw   s1,  36(sp)        /* x9   [9*4]  */
    sw   a0,  40(sp)        /* x10  [10*4] */
    sw   a1,  44(sp)        /* x11  [11*4] */
    sw   a2,  48(sp)        /* x12  [12*4] */
    sw   a3,  52(sp)        /* x13  [13*4] */
    sw   a4,  56(sp)        /* x14  [14*4] */
    sw   a5,  60(sp)        /* x15  [15*4] */
    sw   a6,  64(sp)        /* x16  [16*4] */
    sw   a7,  68(sp)        /* x17  [17*4] */
    sw   s2,  72(sp)        /* x18  [18*4] */
    sw   s3,  76(sp)        /* x19  [19*4] */
    sw   s4,  80(sp)        /* x20  [20*4] */
    sw   s5,  84(sp)        /* x21  [21*4] */
    sw   s6,  88(sp)        /* x22  [22*4] */
    sw   s7,  92(sp)        /* x23  [23*4] */
    sw   s8,  96(sp)        /* x24  [24*4] */
    sw   s9,  100(sp)       /* x25  [25*4] */
    sw   s10, 104(sp)       /* x26  [26*4] */
    sw   s11, 108(sp)       /* x27  [27*4] */
    sw   t3,  112(sp)       /* x28  [28*4] */
    sw   t4,  116(sp)       /* x29  [29*4] */
    sw   t5,  120(sp)       /* x30  [30*4] */
    sw   t6,  124(sp)       /* x31  [31*4] */

    /* save mepc and mstatus */
    csrr t0, mepc
    sw   t0, 0(sp)          /* mepc     [0*4] */
    csrr t0, mstatus
    sw   t0, 4(sp)          /* mstatus  [1*4] */

    /* --- save sp to current TCB ---------------------------------------- */
    la   t0, g_current
    lw   t0, 0(t0)
    sw   sp, 0(t0)

    /* --- dispatch on mcause -------------------------------------------- */
    csrr a0, mcause

    /* machine timer interrupt: 0x80000007 */
    li   t1, 0x80000007
    beq  a0, t1, .Ltimer_isr

    /* machine software interrupt: 0x80000003 */
    li   t1, 0x80000003
    beq  a0, t1, .Lsoft_isr

    /* spurious / unhandled trap — restore and return without switching */
    j    .Lreload_sp

.Ltimer_isr:
    call port_reset_timer       /* reprogram MTIMECMP                     */
    call rtos_tick_handler      /* advance tick, unblock delayed tasks    */
    call rtos_context_switch    /* pick next task, update g_current       */
    j    .Lreload_sp

.Lsoft_isr:
    /* clear MSIP so the interrupt doesn't re-fire */
    li   t0, 0x02000000         /* CLINT_BASE — matches RTOS_CLINT_BASE_ADDR */
    sw   zero, 0(t0)
    call rtos_context_switch

.Lreload_sp:
    /* reload sp from (possibly new) g_current->sp */
    la   t0, g_current
    lw   t0, 0(t0)
    lw   sp, 0(t0)

.Lrestore_and_mret:
    /* restore mepc and mstatus */
    lw   t0, 0(sp)
    csrw mepc, t0
    lw   t0, 4(sp)
    csrw mstatus, t0

    /* restore GPRs */
    lw   ra,   8(sp)
    lw   gp,  12(sp)
    lw   tp,  16(sp)
    lw   t0,  20(sp)
    lw   t1,  24(sp)
    lw   t2,  28(sp)
    lw   s0,  32(sp)
    lw   s1,  36(sp)
    lw   a0,  40(sp)
    lw   a1,  44(sp)
    lw   a2,  48(sp)
    lw   a3,  52(sp)
    lw   a4,  56(sp)
    lw   a5,  60(sp)
    lw   a6,  64(sp)
    lw   a7,  68(sp)
    lw   s2,  72(sp)
    lw   s3,  76(sp)
    lw   s4,  80(sp)
    lw   s5,  84(sp)
    lw   s6,  88(sp)
    lw   s7,  92(sp)
    lw   s8,  96(sp)
    lw   s9,  100(sp)
    lw   s10, 104(sp)
    lw   s11, 108(sp)
    lw   t3,  112(sp)
    lw   t4,  116(sp)
    lw   t5,  120(sp)
    lw   t6,  124(sp)

    addi sp, sp, 128
    mret

/* Copyright 2025. All rights reserved.
 * ESP32-S3 (Xtensa LX7) startup, vector table, and context switch.
 * Call0 ABI — no register window rotation.
 *
 * Boot sequence:
 *   ROM bootloader → _start → clears BSS, copies .data → calls rtos_boot()
 *   rtos_boot() calls port_init() then vRTOSStart()
 *
 * Interrupt vector layout (VECBASE must be 512-byte/0x200 aligned):
 *   VECBASE + 0x000  WindowOverflow4    (4 bytes, unused with Call0, but slot must exist)
 *   VECBASE + 0x020  Level-1 vector     ← our tick / reschedule ISR
 *   VECBASE + 0x040  Level-2 vector     (unused — j . infinite loop)
 *   VECBASE + 0x060  Level-3 vector     (unused)
 *   VECBASE + 0x080  Level-4 vector     (unused)
 *   VECBASE + 0x0A0  Level-5 vector     (unused)
 *   VECBASE + 0x0C0  Debug vector       (unused)
 *   VECBASE + 0x0E0  NMI vector         (unused)
 *   VECBASE + 0x100  Kernel exception   (unused — loops forever)
 *   VECBASE + 0x180  User exception     (unused)
 *   VECBASE + 0x1C0  Double exception   (unused)
 *
 * Context frame (Call0 ABI, 18 words = 72 bytes, sp → frame[0]):
 *   [sp + 0*4]   EPC1  (PC at interrupt)
 *   [sp + 1*4]   EPS1  (PS at interrupt; initial=0 → rfi re-enables interrupts)
 *   [sp + 2*4]   SAR   (Shift Amount Register)
 *   [sp + 3*4]   a0    (return address)
 *   [sp + 4*4]   a2    (first arg in Call0 ABI — task argument at task start)
 *   [sp + 5*4]   a3
 *   ...
 *   [sp +17*4]   a15
 *   a1 (sp) is stored in the TCB (g_current->sp), not in the frame.
 */

/* External C symbols */
    .extern g_current
    .extern rtos_tick_handler
    .extern rtos_context_switch
    .extern port_clear_tick_irq
    .extern port_init
    .extern vRTOSStart

/* -----------------------------------------------------------------------
 * .vecbase section — MUST be at 0x40370000 (or whatever VECBASE is set to).
 * Each slot is exactly 0x20 (32) bytes; vectors jump to the real handler.
 * ----------------------------------------------------------------------- */

    .section .vecbase, "ax"
    .align  0x200          /* VECBASE must be 512-byte aligned             */

    /* Offset 0x000 — WindowOverflow4 (unused with Call0 ABI) */
    .org    0x000
    j       .

    /* Offset 0x020 — Level-1 interrupt vector (tick + reschedule) */
    .org    0x020
    j       _level1_isr

    /* Offset 0x040 — Level-2 */
    .org    0x040
    j       .

    /* Offset 0x060 — Level-3 */
    .org    0x060
    j       .

    /* Offset 0x080 — Level-4 */
    .org    0x080
    j       .

    /* Offset 0x0A0 — Level-5 */
    .org    0x0A0
    j       .

    /* Offset 0x0C0 — Debug */
    .org    0x0C0
    j       .

    /* Offset 0x0E0 — NMI */
    .org    0x0E0
    j       .

    /* Offset 0x100 — Kernel exception (basic fault handler) */
    .org    0x100
    j       .

    /* Offset 0x180 — User exception */
    .org    0x180
    j       .

    /* Offset 0x1C0 — Double exception */
    .org    0x1C0
    j       .

/* -----------------------------------------------------------------------
 * Text section — _start, level-1 ISR, port_start_first_task
 * ----------------------------------------------------------------------- */

    .section .text
    .literal_position          /* emit literal pool here for l32r targets */

/* -----------------------------------------------------------------------
 * _start — reset / entry point.
 * Called by the ROM bootloader (or loaded directly by QEMU).
 * Sets up VECBASE, stack pointer, clears BSS, copies .data, calls rtos_boot.
 * ----------------------------------------------------------------------- */
    .global _start
    .type   _start, @function
_start:
    /* Point VECBASE at our vector table (start of IRAM / .vecbase). */
    movi    a2, _vecbase_start
    wsr.vecbase a2

    /* Set up stack pointer to top of DRAM. */
    movi    sp, _stack_top

    /* Clear BSS. */
    movi    a2, _bss_start
    movi    a3, _bss_end
    movi    a4, 0
.bss_loop:
    bge     a2, a3, .bss_done
    s32i    a4, a2, 0
    addi    a2, a2, 4
    j       .bss_loop
.bss_done:

    /* Copy initialised .data from IRAM load address to DRAM run address. */
    movi    a2, _data_load_start
    movi    a3, _data_start
    movi    a4, _data_end
.data_loop:
    bge     a3, a4, .data_done
    l32i    a5, a2, 0
    s32i    a5, a3, 0
    addi    a2, a2, 4
    addi    a3, a3, 4
    j       .data_loop
.data_done:

    /* Call rtos_boot() — sets up timer, interrupt matrix, then vRTOSStart(). */
    call0   rtos_boot
    /* Should never return; loop if it does. */
.start_done:
    j       .start_done

/* Symbol defined in linker script for the start of the vector table. */
    .global _vecbase_start
_vecbase_start = 0x40370000

/* -----------------------------------------------------------------------
 * rtos_boot — C-callable wrapper called from _start.
 * Initialises the port and starts the scheduler.
 * Defined here so it is in IRAM and callable via call0.
 * ----------------------------------------------------------------------- */
    .global rtos_boot
    .type   rtos_boot, @function
rtos_boot:
    /* port_init(RTOS_TICK_RATE_HZ) */
    movi    a2, 1000        /* RTOS_TICK_RATE_HZ default */
    call0   port_init
    /* vRTOSStart() — never returns */
    call0   vRTOSStart
    j       .

/* -----------------------------------------------------------------------
 * _level1_isr — level-1 interrupt handler (tick + reschedule).
 *
 * On Xtensa level-1 interrupt entry (hardware has already done):
 *   EPC1 ← interrupted PC
 *   EPS1 ← interrupted PS
 *   INTLEVEL in PS ← 1 (blocks further level-1 interrupts)
 *
 * We save the full Call0 context frame (72 bytes) onto the interrupted
 * task's stack, then call the C scheduler, then restore the new task.
 * ----------------------------------------------------------------------- */
    .type   _level1_isr, @function
_level1_isr:
    /* --- Save context -------------------------------------------------- */
    addi    sp, sp, -72         /* allocate 18-word (72-byte) frame        */

    s32i    a0,  sp, 12         /* a0 at offset 3*4=12                     */
    /* Save a2-a15 */
    s32i    a2,  sp, 16
    s32i    a3,  sp, 20
    s32i    a4,  sp, 24
    s32i    a5,  sp, 28
    s32i    a6,  sp, 32
    s32i    a7,  sp, 36
    s32i    a8,  sp, 40
    s32i    a9,  sp, 44
    s32i    a10, sp, 48
    s32i    a11, sp, 52
    s32i    a12, sp, 56
    s32i    a13, sp, 60
    s32i    a14, sp, 64
    s32i    a15, sp, 68

    /* Save EPC1, EPS1, SAR */
    rsr.epc1    a2
    s32i        a2, sp, 0       /* EPC1 at offset 0                        */
    rsr.eps1    a2
    s32i        a2, sp, 4       /* EPS1 at offset 4                        */
    rsr.sar     a2
    s32i        a2, sp, 8       /* SAR  at offset 8                        */

    /* --- Save sp to current TCB ---------------------------------------- */
    movi    a2, g_current
    l32i    a2, a2, 0           /* a2 = g_current (TCB *)                  */
    s32i    sp, a2, 0           /* g_current->sp = sp (void* is 1st field) */

    /* --- Service the timer interrupt if it fired ----------------------- */
    /* Check TIMG0 INT_ST bit 0; if set it is a real tick, not just a sw reschedule. */
    movi    a2, 0x6001F07C      /* TIMG_INT_ST register address            */
    l32i    a2, a2, 0
    andi    a2, a2, 1
    beqz    a2, .no_tick        /* branch if timer int not pending         */

    call0   port_clear_tick_irq /* clear and re-arm TIMG0_T0               */
    call0   rtos_tick_handler   /* advance tick, unblock delayed tasks     */

.no_tick:
    call0   rtos_context_switch /* pick next task, update g_current        */

    /* --- Clear any pending software interrupt on our slot -------------- */
    movi    a2, 1
    slli    a2, a2, 6           /* bit 6 = CPU_INT_SLOT                    */
    wsr.intclear a2

    /* --- Reload sp from (possibly new) g_current ----------------------- */
    movi    a2, g_current
    l32i    a2, a2, 0           /* a2 = (new) g_current                    */
    l32i    sp, a2, 0           /* sp = new task's saved sp                */

.Lrestore_and_rfi:
    /* --- Restore EPC1, EPS1, SAR --------------------------------------- */
    l32i    a2, sp, 0
    wsr.epc1    a2
    l32i    a2, sp, 4
    wsr.eps1    a2
    l32i    a2, sp, 8
    wsr.sar     a2

    /* --- Restore general-purpose registers ----------------------------- */
    l32i    a0,  sp, 12
    l32i    a2,  sp, 16
    l32i    a3,  sp, 20
    l32i    a4,  sp, 24
    l32i    a5,  sp, 28
    l32i    a6,  sp, 32
    l32i    a7,  sp, 36
    l32i    a8,  sp, 40
    l32i    a9,  sp, 44
    l32i    a10, sp, 48
    l32i    a11, sp, 52
    l32i    a12, sp, 56
    l32i    a13, sp, 60
    l32i    a14, sp, 64
    l32i    a15, sp, 68

    addi    sp, sp, 72
    rfi     1                   /* restore PS from EPS1, PC from EPC1      */

/* -----------------------------------------------------------------------
 * port_start_first_task
 * Loads g_current->sp and restores the initial context of the first task.
 * Falls through to the shared restore+rfi path.
 * ----------------------------------------------------------------------- */
    .global port_start_first_task
    .type   port_start_first_task, @function
port_start_first_task:
    movi    a2, g_current
    l32i    a2, a2, 0           /* a2 = g_current (TCB *)                  */
    l32i    sp, a2, 0           /* sp = g_current->sp                      */
    j       .Lrestore_and_rfi

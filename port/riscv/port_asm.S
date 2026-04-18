/* Copyright 2025. All rights reserved.
 * RISC-V context switch — placeholder, to be implemented.
 */
    .section .text
    .global riscv_trap_handler
riscv_trap_handler:
    /* TODO: save all registers, call rtos_tick_handler / reschedule */
    mret

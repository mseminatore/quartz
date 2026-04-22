// Copyright 2025. All rights reserved.
// RISC-V (RV32I/RV32IMAC) machine-mode port.
//
// Target: bare-metal RISC-V with a standard CLINT (Core-Local Interruptor).
// Tested on QEMU "virt" machine (CLINT at 0x02000000, MTIME at 10 MHz).
// Compatible with SiFive FE310 / HiFive1 — set RTOS_MTIME_HZ=32768 for that.
//
// Context frame (128 bytes, 32 words, sp points to lowest address):
//
//   offset  register / CSR
//   ------  ---------------
//    [0]    mepc       (task PC, restored with csrw / loaded by mret)
//    [1]    mstatus    (initial: MPP=11 M-mode, MPIE=1 → mret re-enables intr.)
//    [2]    x1  (ra)
//    [3]    x3  (gp)
//    [4]    x4  (tp)
//    [5]    x5  (t0)
//    [6]    x6  (t1)
//    [7]    x7  (t2)
//    [8]    x8  (s0/fp)
//    [9]    x9  (s1)
//   [10]    x10 (a0)   ← initial value = task argument
//   [11]    x11 (a1)
//   [12]    x12 (a2)
//   [13]    x13 (a3)
//   [14]    x14 (a4)
//   [15]    x15 (a5)
//   [16]    x16 (a6)
//   [17]    x17 (a7)
//   [18]    x18 (s2)
//   [19]    x19 (s3)
//   [20]    x20 (s4)
//   [21]    x21 (s5)
//   [22]    x22 (s6)
//   [23]    x23 (s7)
//   [24]    x24 (s8)
//   [25]    x25 (s9)
//   [26]    x26 (s10)
//   [27]    x27 (s11)
//   [28]    x28 (t3)
//   [29]    x29 (t4)
//   [30]    x30 (t5)
//   [31]    x31 (t6)
//
// x0 (zero) is not saved. x2 (sp) is saved in the TCB (g_current->sp).
#include "../../src/port.h"
#include "../../include/rtos_config.h"
#include <stdint.h>
#include <string.h>

// Trap handler defined in port_asm.S.
extern void riscv_trap_handler(void);

// CLINT register layout.
#define CLINT_MSIP      (*(volatile uint32_t *)(RTOS_CLINT_BASE_ADDR + 0x0000UL))
#define CLINT_MTIMECMP  (*(volatile uint64_t *)(RTOS_CLINT_BASE_ADDR + 0x4000UL))
#define CLINT_MTIME     (*(volatile uint64_t *)(RTOS_CLINT_BASE_ADDR + 0xBFF8UL))

// Ticks between MTIME comparisons.
#define MTIME_TICKS_PER_TICK  ((uint64_t)(RTOS_MTIME_HZ / RTOS_TICK_RATE_HZ))

// Called from the trap handler (and externally) to reprogram the next compare.
void port_reset_timer(void)
{
    CLINT_MTIMECMP = CLINT_MTIME + MTIME_TICKS_PER_TICK;
}

void port_enter_critical(void)
{
    __asm__ volatile("csrci mstatus, 0x8");
}

void port_exit_critical(void)
{
    __asm__ volatile("csrsi mstatus, 0x8");
}

void port_request_reschedule(void)
{
    // Trigger machine software interrupt — trap handler clears it.
    CLINT_MSIP = 1;
}

void port_init(uint32_t tick_rate_hz)
{
    (void)tick_rate_hz; // rate is set by RTOS_TICK_RATE_HZ / RTOS_MTIME_HZ

    // Point mtvec at our trap handler (direct mode — bit 0 = 0).
    __asm__ volatile("csrw mtvec, %0" :: "r"(riscv_trap_handler));

    // Clear any pending software interrupt.
    CLINT_MSIP = 0;

    // Programme first timer compare.
    port_reset_timer();

    // Enable machine timer interrupt (MTIE, bit 7) and machine software
    // interrupt (MSIE, bit 3) in mie.
    __asm__ volatile("csrs mie, %0" :: "r"(0x88u));

    // Enable global interrupts (MIE bit 3 in mstatus).
    __asm__ volatile("csrsi mstatus, 0x8");
}

// mstatus initial value: MPP=11 (remain in M-mode after mret), MPIE=1 (mret
// re-enables interrupts).  Bit positions: MPP[12:11]=0b11, MPIE[7]=1.
#define INIT_MSTATUS  ((uint32_t)0x1880u)

// Frame is 32 words = 128 bytes.
#define FRAME_WORDS   32
#define FRAME_BYTES   (FRAME_WORDS * 4)

void *port_init_stack(void     *stack_top,
                      void    (*func)(void *),
                      void     *arg)
{
    uint32_t *sp = (uint32_t *)((uint8_t *)stack_top - FRAME_BYTES);
    memset(sp, 0, FRAME_BYTES);

    sp[0]  = (uint32_t)func;   // mepc  → task entry point
    sp[1]  = INIT_MSTATUS;     // mstatus
    sp[10] = (uint32_t)arg;    // x10 (a0) → task argument

    return sp;
}

// port_start_first_task is implemented in port_asm.S.

// ---------------------------------------------------------------------------
// port_cpu_idle — WFI on RISC-V; waits for the next machine interrupt.
// ---------------------------------------------------------------------------

void port_cpu_idle(void)
{
    __asm__ volatile("wfi");
}

// ---------------------------------------------------------------------------
// port_core_id — returns the hart ID from the mhartid CSR.
// On single-hart targets this is always 0.
// ---------------------------------------------------------------------------

uint8_t port_core_id(void)
{
    uint32_t id;
    __asm__ volatile("csrr %0, mhartid" : "=r"(id));
    return (uint8_t)id;
}

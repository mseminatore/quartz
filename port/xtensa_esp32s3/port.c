// Copyright 2025. All rights reserved.
// ESP32-S3 (Xtensa LX7) port — Call0 ABI, bare-metal, QEMU compatible.
//
// Tick source: Timer Group 0, Timer 0 (TIMG0_T0).
//   APB clock = 80 MHz.  Divider = 80 → 1 MHz counter.  Alarm = 999 → 1 kHz tick.
//
// Interrupt routing:
//   Peripheral source 10 (TG0_T0_LEVEL_INT) → Interrupt Matrix → CPU slot 6.
//   CPU slot 6 is configured as a level-1 edge-triggered interrupt.
//   VECBASE register points to the vector table in IRAM (.vecbase section).
//   Level-1 interrupt vector = VECBASE + 0x020.
//
// Critical sections: RSIL (Read/Set Interrupt Level) saves and raises INTLEVEL
//   to 15 (blocks all interrupts up to the NMI threshold).
//   WSR restores the previous PS value to re-enable interrupts.
//
// Context frame (Call0 ABI, 18 words = 72 bytes, sp points to lowest address):
//
//   offset  register
//   ------  --------
//    [0]    EPC1    (PC saved by hardware on level-1 interrupt)
//    [1]    EPS1    (PS saved by hardware on level-1 interrupt)
//    [2]    SAR     (Shift Amount Register)
//    [3]    a0      (return address / link register)
//    [4]    a2      (first task argument in Call0 ABI)
//    [5]    a3
//    ...
//   [17]    a15
//
//   a1 (sp) is saved in tcb->sp (not in the frame).
//   Initial EPS1 = 0x00000000 (INTLEVEL=0, WOE=0, supervisor mode)
//   so that rfi 1 re-enables all interrupts when a task first runs.

#include "../../src/port.h"
#include "../../include/rtos_config.h"
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// TIMG0 register layout (Timer Group 0, base = RTOS_TIMG0_BASE_ADDR)
// ---------------------------------------------------------------------------
#define TIMG_BASE       RTOS_TIMG0_BASE_ADDR

#define TIMG_T0CONFIG   (*(volatile uint32_t *)(TIMG_BASE + 0x0000UL))
#define TIMG_T0LO       (*(volatile uint32_t *)(TIMG_BASE + 0x0004UL))
#define TIMG_T0HI       (*(volatile uint32_t *)(TIMG_BASE + 0x0008UL))
#define TIMG_T0UPDATE   (*(volatile uint32_t *)(TIMG_BASE + 0x000CUL))
#define TIMG_T0ALARMLO  (*(volatile uint32_t *)(TIMG_BASE + 0x0010UL))
#define TIMG_T0ALARMHI  (*(volatile uint32_t *)(TIMG_BASE + 0x0014UL))
#define TIMG_T0LOADLO   (*(volatile uint32_t *)(TIMG_BASE + 0x0018UL))
#define TIMG_T0LOADHI   (*(volatile uint32_t *)(TIMG_BASE + 0x001CUL))
#define TIMG_T0LOAD     (*(volatile uint32_t *)(TIMG_BASE + 0x0020UL))
#define TIMG_INT_ENA    (*(volatile uint32_t *)(TIMG_BASE + 0x0074UL))
#define TIMG_INT_RAW    (*(volatile uint32_t *)(TIMG_BASE + 0x0078UL))
#define TIMG_INT_ST     (*(volatile uint32_t *)(TIMG_BASE + 0x007CUL))
#define TIMG_INT_CLR    (*(volatile uint32_t *)(TIMG_BASE + 0x0080UL))

// TIMG_T0CONFIG bit fields
#define TIMG_T0_EN          (1u << 31)
#define TIMG_T0_INCREASE    (1u << 30)
#define TIMG_T0_AUTORELOAD  (1u << 29)
#define TIMG_T0_DIVIDER(n)  (((n) & 0xFFFFu) << 13)
#define TIMG_T0_ALARM_EN    (1u << 10)
#define TIMG_T0_LEVEL_INT_EN (1u << 2)

// Divisor for 1 MHz from 80 MHz APB, then alarm count for desired tick rate.
#define TIMG_DIVIDER        80u
#define TIMG_ALARM_COUNT    ((1000000u / RTOS_TICK_RATE_HZ) - 1u)

// ---------------------------------------------------------------------------
// Interrupt Matrix — routes peripheral interrupt sources to CPU slots.
// Base address for CPU0's interrupt map registers: 0x600C2000.
// TG0_T0_LEVEL_INT = source 10.  We map it to CPU interrupt slot 6.
// ---------------------------------------------------------------------------
#define INTMATRIX_BASE  0x600C2000UL
// Offset for source 10 (TG0_T0_LEVEL_INT): each source has one 32-bit register,
// first source at offset 0x004, so source 10 is at offset 0x004 + 9*4 = 0x028.
// (Source numbering starts at 1 in ESP32-S3 TRM; offset for source N = (N-1)*4 + 4)
#define TIMG0_T0_INT_MAP (*(volatile uint32_t *)(INTMATRIX_BASE + 0x028UL))
#define CPU_INT_SLOT     6u     /* arbitrary level-1 slot; must match port_asm.S */

// INTENABLE CSR — enables per-slot interrupts in the CPU.
// Accessed via WSR/RSR Xtensa instructions (CSR 0x604 = INTENABLE).
static inline void cpu_intenable_set(uint32_t mask)
{
    uint32_t cur;
    __asm__ volatile("rsr.intenable %0" : "=a"(cur));
    cur |= mask;
    __asm__ volatile("wsr.intenable %0" :: "a"(cur));
}

// ---------------------------------------------------------------------------
// Critical sections using RSIL (Read/Set Interrupt Level)
// ---------------------------------------------------------------------------

// Each task's critical-nesting count (one per task; simple since single-core).
// For simplicity, we use a global nesting counter (single-core ESP32-S3 use case).
static uint32_t g_saved_ps;
static uint32_t g_critical_nesting;

void port_enter_critical(void)
{
    uint32_t ps;
    __asm__ volatile("rsil %0, 15" : "=a"(ps));   // raise INTLEVEL to 15, save old PS
    if (g_critical_nesting == 0) {
        g_saved_ps = ps;
    }
    g_critical_nesting++;
}

void port_exit_critical(void)
{
    if (g_critical_nesting > 0) {
        g_critical_nesting--;
    }
    if (g_critical_nesting == 0) {
        __asm__ volatile("wsr.ps %0; rsync" :: "a"(g_saved_ps));
    }
}

// ---------------------------------------------------------------------------
// Reschedule request — triggers a software-level interrupt via INTSET CSR.
// We use the same CPU slot as the timer so the same ISR handles both paths.
// (The ISR checks whether it's a real tick by reading TIMG_INT_ST.)
// ---------------------------------------------------------------------------

void port_request_reschedule(void)
{
    // INTSET (CSR 0xE06) — writing a 1 to a bit forces the corresponding
    // software interrupt.  Use the same slot so the ISR is shared.
    __asm__ volatile("wsr.intset %0" :: "a"(1u << CPU_INT_SLOT));
}

// ---------------------------------------------------------------------------
// Clear the TIMG0_T0 interrupt and re-arm the alarm.
// Called from the level-1 ISR (defined in port_asm.S).
// ---------------------------------------------------------------------------

void port_clear_tick_irq(void)
{
    TIMG_INT_CLR = 1u;         // clear T0 interrupt flag
    TIMG_T0CONFIG |= TIMG_T0_ALARM_EN;  // re-arm alarm (auto-reload handles counter reset)
}

// ---------------------------------------------------------------------------
// Stack initialisation (Call0 ABI context frame)
// ---------------------------------------------------------------------------

// Frame layout constants (words from bottom of frame):
#define FRAME_WORDS     18u
#define FRAME_BYTES     (FRAME_WORDS * 4u)

// Offsets within the frame (word index × 4 = byte offset from sp):
#define OFF_EPC1    (0u * 4u)
#define OFF_EPS1    (1u * 4u)
#define OFF_SAR     (2u * 4u)
#define OFF_A0      (3u * 4u)
#define OFF_A2      (4u * 4u)   /* first argument in Call0 ABI */

void *port_init_stack(void     *stack_top,
                      void    (*func)(void *),
                      void     *arg)
{
    uint32_t *sp = (uint32_t *)((uint8_t *)stack_top - FRAME_BYTES);
    memset(sp, 0, FRAME_BYTES);

    // EPC1 = entry point of the task function.
    ((uint8_t *)sp)[OFF_EPC1 + 0] = ((uint32_t)func >>  0) & 0xFF;
    ((uint8_t *)sp)[OFF_EPC1 + 1] = ((uint32_t)func >>  8) & 0xFF;
    ((uint8_t *)sp)[OFF_EPC1 + 2] = ((uint32_t)func >> 16) & 0xFF;
    ((uint8_t *)sp)[OFF_EPC1 + 3] = ((uint32_t)func >> 24) & 0xFF;

    // EPS1 = 0x00000000: INTLEVEL=0 (all interrupts enabled after rfi),
    //                    WOE=0 (Call0 — no window overflow), UM=0 (supervisor).
    // (Already zeroed by memset.)

    // a2 = first argument (Call0 ABI).
    ((uint8_t *)sp)[OFF_A2 + 0] = ((uint32_t)arg >>  0) & 0xFF;
    ((uint8_t *)sp)[OFF_A2 + 1] = ((uint32_t)arg >>  8) & 0xFF;
    ((uint8_t *)sp)[OFF_A2 + 2] = ((uint32_t)arg >> 16) & 0xFF;
    ((uint8_t *)sp)[OFF_A2 + 3] = ((uint32_t)arg >> 24) & 0xFF;

    return sp;
}

// ---------------------------------------------------------------------------
// Port initialisation
// ---------------------------------------------------------------------------

void port_init(uint32_t tick_rate_hz)
{
    (void)tick_rate_hz;   // rate is fixed by TIMG_DIVIDER / TIMG_ALARM_COUNT

    // --- Configure TIMG0_T0 ------------------------------------------------
    // Reset counter to 0.
    TIMG_T0LOADLO = 0;
    TIMG_T0LOADHI = 0;
    TIMG_T0LOAD   = 1;   // writing any value triggers the load

    // Alarm value (64-bit, upper 32 bits = 0 since we reset on every alarm).
    TIMG_T0ALARMLO = TIMG_ALARM_COUNT;
    TIMG_T0ALARMHI = 0;

    // Enable timer: up-counting, auto-reload, set prescaler, enable alarm.
    TIMG_T0CONFIG = TIMG_T0_EN
                  | TIMG_T0_INCREASE
                  | TIMG_T0_AUTORELOAD
                  | TIMG_T0_DIVIDER(TIMG_DIVIDER)
                  | TIMG_T0_ALARM_EN
                  | TIMG_T0_LEVEL_INT_EN;

    // Enable T0 interrupt in TIMG interrupt enable register (bit 0 = T0).
    TIMG_INT_ENA |= 1u;

    // --- Configure Interrupt Matrix ----------------------------------------
    // Route TG0_T0_LEVEL_INT (source 10) to CPU interrupt slot CPU_INT_SLOT.
    TIMG0_T0_INT_MAP = CPU_INT_SLOT;

    // Enable the CPU interrupt slot via INTENABLE CSR.
    cpu_intenable_set(1u << CPU_INT_SLOT);

    // Global interrupt enable is handled by port_start_first_task via rfi 1
    // (which restores EPS1 = 0, setting INTLEVEL=0 and enabling interrupts).
}

// port_start_first_task is implemented in port_asm.S.

// ---------------------------------------------------------------------------
// port_cpu_idle — use WAITI 0 to wait until an enabled interrupt arrives.
// WAITI sets INTLEVEL=0 (all enabled interrupts can fire) and halts the CPU.
// ---------------------------------------------------------------------------

void port_cpu_idle(void)
{
    __asm__ volatile("waiti 0");
}

// ---------------------------------------------------------------------------
// port_core_id — returns the current CPU core index from the PRID register.
// On ESP32-S3 PRID bit 13 distinguishes PRO_CPU (0) from APP_CPU (1).
// ---------------------------------------------------------------------------

uint8_t port_core_id(void)
{
#if RTOS_NUM_CORES > 1
    uint32_t prid;
    __asm__ volatile("rsr.prid %0" : "=a"(prid));
    return (uint8_t)((prid >> 13) & 1u);
#else
    return 0;
#endif
}

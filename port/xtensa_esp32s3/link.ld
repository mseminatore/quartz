/* Minimal linker script for ESP32-S3 (Xtensa LX7), Call0 ABI, bare-metal.
 *
 * Memory map:
 *   IRAM  0x40370000  512 KB   instruction + read-only RAM (fast, XIP-capable)
 *   DRAM  0x3FC88000  384 KB   data RAM
 *
 * For QEMU (qemu-system-xtensa -machine esp32s3 -kernel rtos.elf) the ELF
 * is loaded directly into physical memory; no flash or MMU mapping required.
 *
 * Vector table alignment: VECBASE must be 512-byte (0x200) aligned.
 * The interrupt vectors are at fixed offsets from VECBASE:
 *   +0x000  WindowOverflow4   (unused with Call0 ABI — but still must exist)
 *   +0x020  Level-1 interrupt vector  ← our tick / reschedule ISR
 *   +0x040  Level-2 vector            (unused)
 *   ...
 *   +0x1C0  KernelExceptionVector     (basic fault handler)
 */

ENTRY(_start)

MEMORY
{
    IRAM (rwx) : ORIGIN = 0x40370000, LENGTH = 512K
    DRAM (rw)  : ORIGIN = 0x3FC88000, LENGTH = 384K
}

SECTIONS
{
    /* -----------------------------------------------------------------
     * Vector table — must be first in IRAM and 512-byte aligned so
     * VECBASE can point here.
     * ----------------------------------------------------------------- */
    .vecbase 0x40370000 :
    {
        KEEP(*(.vecbase))
        KEEP(*(.vecbase.*))
    } > IRAM

    /* -----------------------------------------------------------------
     * Code and read-only data in IRAM
     * ----------------------------------------------------------------- */
    .text :
    {
        *(.text._start)
        *(.text*)
        *(.literal*)           /* Xtensa literal pools (l32r targets) */
    } > IRAM

    .rodata :
    {
        *(.rodata*)
    } > IRAM

    /* -----------------------------------------------------------------
     * Initialised data: stored in IRAM (load address), copied to DRAM
     * (run address) by the startup code.
     * ----------------------------------------------------------------- */
    .data :
    {
        _data_start = .;
        *(.data*)
        _data_end = .;
    } > DRAM AT > IRAM

    _data_load_start = LOADADDR(.data);

    /* -----------------------------------------------------------------
     * Zero-initialised data: cleared by startup code.
     * ----------------------------------------------------------------- */
    .bss :
    {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    } > DRAM

    /* Stack grows down from the top of DRAM. */
    _stack_top = ORIGIN(DRAM) + LENGTH(DRAM);
}

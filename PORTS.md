
### RP2040 (requires [pico-sdk](https://github.com/raspberrypi/pico-sdk))

```sh
export PICO_SDK_PATH=~/pico-sdk
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### AVR ATmega328P (Arduino Uno / Nano)

Requires `avr-gcc`, `avr-libc`, and `avrdude`.  On macOS: `brew install avr-gcc avrdude`.

```sh
# Cross-compile for ATmega328P at 16 MHz
cmake -B build_avr \
      -DCMAKE_TOOLCHAIN_FILE=cmake/avr_atmega328p.cmake
cmake --build build_avr

# Flash via an Arduino-style bootloader (adjust port as needed)
avrdude -c arduino -p m328p -P /dev/ttyUSB0 -b 115200 \
        -U flash:w:build_avr/rtos.hex
```

**Memory constraints (2 KB SRAM):** Override the defaults in your application by
defining these *before* including `rtos.h`:

```c
#define RTOS_MAX_TASKS           4   // keep total TCB count low
#define RTOS_MAX_TIMERS          4
#define RTOS_IDLE_STACK_WORDS   16   // 16 bytes for the idle stack
#define RTOS_STACK_BYTES_PER_WORD 1  // one byte per "word" on AVR
```

User task stacks should be declared as `uint8_t` arrays:

```c
static rtos_tcb_t my_tcb;
static uint8_t    my_stack[64];   // 64 bytes

rtos_task_create(&my_tcb, my_stack, 64, my_task_func, NULL, "myTask", 1);
```

The AVR port is fully preemptive: in addition to the periodic Timer1 OCR1A tick,
`port_request_reschedule()` arms a Timer1 OCR1B compare-match one count in the future
so that an in-task or ISR call to `rtos_semaphore_give`/`rtos_queue_send` (etc.) that
unblocks a higher-priority task triggers a context switch immediately, without waiting
for the next tick.

### RISC-V RV32IMAC (QEMU virt / SiFive FE310 / HiFive1)

Requires `riscv64-unknown-elf-gcc` (targets RV32 via `-march=rv32imac`).
On macOS: `brew install riscv-software-src/riscv/riscv-gnu-toolchain`.
On Ubuntu: `sudo apt install gcc-riscv64-unknown-elf`.

```sh
# Cross-compile for RV32IMAC (QEMU virt machine — CLINT at 0x02000000, MTIME 10 MHz)
cmake -B build_rv32 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/riscv32_clint.cmake
cmake --build build_rv32

# Run on QEMU virt machine (requires a linker script to place the binary at 0x80000000)
qemu-system-riscv32 -machine virt -nographic -bios none \
                    -kernel build_rv32/rtos.elf
```

**SiFive HiFive1 / FE310 (32 768 Hz MTIME):** override the MTIME frequency in
your application config before including `rtos.h`:

```c
#define RTOS_MTIME_HZ  32768UL   // FE310 MTIME runs at 32.768 kHz
```

### ESP32-S3 (Xtensa LX7 — Espressif QEMU)

Requires Espressif's Xtensa toolchain (`xtensa-esp32s3-elf-gcc`) and the
[Espressif QEMU fork](https://github.com/espressif/qemu/releases).

**Toolchain install (one-time):**
```sh
# Via ESP-IDF (recommended — installs matching QEMU too):
#   https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/
# Or standalone pre-built binaries from Espressif's GitHub releases.
```

```sh
# Cross-compile for ESP32-S3 (Xtensa LX7, Call0 ABI, TIMG0 1 kHz tick)
cmake -B build_esp32s3 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/esp32s3_qemu.cmake
cmake --build build_esp32s3

# Run on Espressif QEMU fork
qemu-system-xtensa -machine esp32s3 -nographic \
                   -kernel build_esp32s3/rtos.elf
```

**Key design notes (ESP32-S3):**
- Uses **Call0 ABI** (`-mabi=call0`): flat register set, no window overflow complexity.
- Tick source: **Timer Group 0, Timer 0** (TIMG0_T0) — APB 80 MHz, divider=80 → 1 MHz, alarm=999 → 1 kHz.
- Interrupt routing: Peripheral source 10 (TG0_T0_LEVEL_INT) → Interrupt Matrix → CPU slot 6 → level-1 ISR.
- Context frame: 18 words (72 bytes) — EPC1, EPS1, SAR, a0, a2–a15. `sp` is stored in the TCB.

### ARM Cortex-M4F (generic — e.g. STM32F4xx)

Requires `arm-none-eabi-gcc`.
On macOS: `brew install arm-none-eabi-gcc`.
On Ubuntu: `sudo apt install gcc-arm-none-eabi`.

```sh
# Cross-compile for generic Cortex-M4F (168 MHz default CPU clock)
cmake -B build_cm4 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm_cm4.cmake
cmake --build build_cm4

# Override CPU clock for a different board (e.g. STM32F3xx at 72 MHz):
cmake -B build_cm4 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm_cm4.cmake \
      -DRTOS_CPU_HZ=72000000
```

You will also need to supply a board-specific linker script (`-T device.ld`) and startup
file (`startup_stm32f4xx.s`) in your application's CMakeLists.  The kernel library
(`librtos.a`) links cleanly without them.

**Key differences from the Cortex-M0+ port:**
- **BASEPRI-based critical sections** (not PRIMASK): interrupts with numeric priority
  lower than `RTOS_BASEPRI_VALUE >> 4` remain active during critical sections, enabling
  safety-critical ISRs to preempt the kernel.  Default threshold is priority 5
  (`RTOS_BASEPRI_VALUE=0x50`); override with `-DRTOS_BASEPRI_VALUE=0x40` etc.
- Any ISR that calls RTOS APIs (e.g. `rtos_semaphore_give_from_isr`) must be
  configured at a numeric priority **≥** `RTOS_BASEPRI_VALUE >> 4`.
- **Simplified assembly**: Thumb-2 `stmdb`/`ldmia` save/restore R4–R11 in a single
  instruction each (no two-step dance needed for R8–R11).
- **FPU support**: Set `-DRTOS_CM4_FPU=1` (or define in `rtos_config.h`) to enable
  per-task save/restore of the FPv4-SP callee-saved registers (S16–S31) and per-task
  EXC_RETURN. Without this option the kernel saves R4–R11 only, so any task that uses
  floating-point instructions can corrupt the FP state of other tasks.

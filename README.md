# rtos

A small, portable, hobby-grade RTOS written in C.

**Design goals:**
- Preemptive, priority-based scheduling (round-robin within equal priorities)
- Static memory allocation only — no `malloc`, no surprises
- Clean `rtos_` prefixed snake_case API (`rtos_task_create`, `rtos_task_delay`, `rtos_semaphore_take`, …)
- Easily portable — architecture-specific code isolated in `port/<arch>/`
- Host-testable kernel logic (unit tests run on the development machine)

**Supported targets:** RP2040 (ARM Cortex-M0+), AVR ATmega328P (Arduino Uno/Nano), RISC-V RV32IMAC (QEMU virt / SiFive FE310), ESP32-S3 (Xtensa LX7, Espressif QEMU)

---

## Repository layout

```
rtos/
├── include/          # Public API headers
│   ├── rtos.h        # Master include
│   ├── rtos_config.h # Compile-time knobs
│   ├── rtos_task.h
│   ├── rtos_sem.h
│   ├── rtos_mutex.h
│   ├── rtos_queue.h
│   └── rtos_timer.h
├── src/              # Architecture-independent kernel
│   ├── task.c        # Scheduler + task management
│   ├── sem.c
│   ├── mutex.c
│   ├── queue.c
│   ├── timer.c
│   └── list.c        # Internal sorted linked list
├── port/
│   ├── arm_cm0plus/       # SysTick, PendSV context switch (RP2040)
│   ├── avr_atmega/        # Timer1 CTC, cli/sei, ISR_NAKED context switch (ATmega328P)
│   ├── riscv/             # CLINT timer, machine-mode trap handler (RV32IMAC)
│   ├── xtensa_esp32s3/    # TIMG0 timer, interrupt matrix, Xtensa level-1 ISR (ESP32-S3)
│   └── host/              # POSIX ucontext simulation port (macOS / Linux)
├── samples/
│   ├── blink.c                  # Single task + delay (LED blink)
│   ├── producer_consumer.c      # Queue: producer sends, consumer receives
│   └── mutex_shared_resource.c  # Mutex: two tasks sharing a counter
├── cmake/
│   ├── avr_atmega328p.cmake   # avr-gcc toolchain file
│   ├── riscv32_clint.cmake    # riscv64-unknown-elf-gcc toolchain file (rv32imac)
│   └── esp32s3_qemu.cmake     # xtensa-esp32s3-elf-gcc toolchain file (Call0 ABI)
├── test/
│   └── test_rtos.c   # Host-side unit tests
└── CMakeLists.txt
```

---

## Configuration

Edit `include/rtos_config.h` (or define before including `rtos.h`):

| Macro | Default | Meaning |
|---|---|---|
| `RTOS_MAX_TASKS` | 16 | Maximum simultaneous tasks (includes idle) |
| `RTOS_MAX_PRIORITIES` | 8 | Number of priority levels (0 = highest) |
| `RTOS_TICK_RATE_HZ` | 1000 | SysTick / timer interrupt frequency |
| `RTOS_MAX_TIMERS` | 8 | Maximum software timers |
| `RTOS_TASK_NAME_LEN` | 16 | Task/timer name buffer size |
| `RTOS_IDLE_STACK_WORDS` | 64 | Idle stack size in `RTOS_STACK_BYTES_PER_WORD` units (256 B on 32-bit) |
| `RTOS_STACK_BYTES_PER_WORD` | 4 | Bytes per stack unit — set to `1` on AVR |
| `RTOS_CLINT_BASE_ADDR` | `0x02000000` | CLINT base address (RISC-V only) |
| `RTOS_MTIME_HZ` | `10000000` | MTIME counter frequency in Hz (RISC-V only) |
| `RTOS_ESP32S3_CPU_HZ` | `240000000` | CPU frequency in Hz (ESP32-S3 only) |
| `RTOS_TIMG0_BASE_ADDR` | `0x6001F000` | Timer Group 0 base address (ESP32-S3 only) |

---

## Building

### Host unit tests (macOS / Linux)

```sh
cmake -B build
cmake --build build
./build/rtos_test
```

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

---

### Kernel
```c
void     rtos_start(void);              // start scheduler — never returns
uint32_t rtos_task_tick_count(void);
```

### Tasks
```c
// All storage (TCB + stack) must be static/global — provided by the caller.
static rtos_tcb_t my_tcb;
static uint32_t   my_stack[256];

rtos_handle_t h = rtos_task_create(&my_tcb, my_stack, 256,
                                   my_task_func, NULL, "myTask", 3);
void rtos_task_delay(uint32_t ticks);   // block for N ticks
void rtos_task_yield(void);
void rtos_task_suspend(rtos_handle_t);
void rtos_task_resume(rtos_handle_t);
void rtos_task_delete(rtos_handle_t);   // pass NULL for current task
```

### Semaphores
```c
static rtos_sem_t my_sem;
rtos_handle_t s = rtos_semaphore_create_binary(&my_sem);
// or:
rtos_handle_t s = rtos_semaphore_create_counting(&my_sem, max, initial);

rtos_semaphore_give(s);
rtos_semaphore_take(s, RTOS_WAIT_FOREVER);   // returns RTOS_OK or RTOS_TIMEOUT
rtos_semaphore_give_from_isr(s);
```

### Mutexes
```c
static rtos_mutex_t my_mutex;
rtos_handle_t m = rtos_mutex_create(&my_mutex);

rtos_mutex_lock(m, RTOS_WAIT_FOREVER);
rtos_mutex_unlock(m);
```

### Message queues
```c
static rtos_queue_t my_queue;
static uint8_t      my_buf[8 * sizeof(int)];   // 8 ints
rtos_handle_t q = rtos_queue_create(&my_queue, my_buf, sizeof(int), 8);

int val = 42;
rtos_queue_send(q, &val, RTOS_WAIT_FOREVER);
rtos_queue_receive(q, &val, RTOS_WAIT_FOREVER);
rtos_queue_send_from_isr(q, &val);
rtos_queue_messages_waiting(q);
```

### Software timers
```c
static rtos_timer_t my_timer;
rtos_handle_t t = rtos_timer_create(&my_timer, "blink", 500, /*periodic=*/1, blink_cb);
rtos_timer_start(t);
rtos_timer_stop(t);
rtos_timer_reset(t);
```

---

## Minimal example (RP2040)

```c
#include "rtos.h"

static rtos_tcb_t task1_tcb, task2_tcb;
static uint32_t   task1_stack[256], task2_stack[256];
static rtos_sem_t ready_sem;

static void task1(void *arg)
{
    rtos_handle_t sem = (rtos_handle_t)arg;
    for (;;) {
        rtos_semaphore_take(sem, RTOS_WAIT_FOREVER);
        // do work
    }
}

static void task2(void *arg)
{
    rtos_handle_t sem = (rtos_handle_t)arg;
    for (;;) {
        rtos_task_delay(1000);          // 1 second at 1000 Hz
        rtos_semaphore_give(sem);
    }
}

int main(void)
{
    rtos_handle_t sem = rtos_semaphore_create_binary(&ready_sem);

    rtos_task_create(&task1_tcb, task1_stack, 256, task1, sem, "task1", 1);
    rtos_task_create(&task2_tcb, task2_stack, 256, task2, sem, "task2", 2);

    rtos_start();   // never returns
}
```

## Samples

Three runnable demos are in `samples/`. They build and run on the host (macOS / Linux) using the `port/host/` simulation port; on RP2040 the hardware stubs are replaced with real GPIO/UART calls via `#ifdef __rp2040__`.

```sh
cmake -B build && cmake --build build
./build/sample_blink                  # single task, LED blink via rtos_task_delay
./build/sample_producer_consumer      # queue: producer sends ints, consumer prints them
./build/sample_mutex_shared_resource  # mutex: two tasks share a counter safely
```

---



1. Copy `port/arm_cm0plus/` (or `port/riscv/`) to `port/<your-arch>/`
2. Implement `port.c`: `port_init`, `port_enter_critical`, `port_exit_critical`, `port_request_reschedule`, `port_start_first_task`, `port_init_stack`
3. Implement `port_asm.S` (or use inline asm in `port.c`): the context-switch handler
4. Add a CMake toolchain file in `cmake/` and update `CMakeLists.txt` to select the port sources

---

## Timeout values

| Constant | Value | Meaning |
|---|---|---|
| `RTOS_WAIT_FOREVER` | `0xFFFFFFFF` | Block indefinitely |
| `RTOS_NO_WAIT` | `0` | Return immediately |

## Return codes

| Constant | Value |
|---|---|
| `RTOS_OK` | `0` |
| `RTOS_ERR` | `-1` |
| `RTOS_TIMEOUT` | `-2` |

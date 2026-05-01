# rtos

[![CI](https://github.com/mseminatore/rtos/actions/workflows/ci.yml/badge.svg)](https://github.com/mseminatore/rtos/actions/workflows/ci.yml)

A small, fast, portable, RTOS written in C.

**Design goals:**
- Preemptive, priority-based scheduling (round-robin within equal priorities)
- Static memory allocation only — no `malloc`, no kernel allocadtions or surprises
- Easily portable — architecture-specific code isolated in `port/<arch>/`
- Host-testable kernel logic (unit tests run on the development machine)
- O(k) tick handler — only examines the *k* tasks expiring on the current tick, not all blocked tasks
- Priority-inheritance mutex — prevents unbounded priority inversion (single-level boost)

**Supported targets:** RP2040 (ARM Cortex-M0+), ARM Cortex-M4F (generic bare-metal, e.g. STM32F4xx), AVR ATmega328P (Arduino Uno/Nano), RISC-V RV32IMAC (QEMU virt / SiFive FE310), ESP32-S3 (Xtensa LX7, Espressif QEMU)

---

## Repository layout

```
rtos/
├── include/          # Public API headers
│   ├── rtos.h        # Master include
│   ├── rtos_config.h # Compile-time knobs
│   ├── rtos_task.h   # Task definitions
│   ├── rtos_sem.h    # Semaphore definitions
│   ├── rtos_mutex.h  # Mutex definitions
│   ├── rtos_queue.h  # Message queue definitions
│   ├── rtos_timer.h  # Software timer definitions
│   └── rtos_trace.h  # Optional trace hook macros
├── src/              # Architecture-independent kernel
│   ├── task.c        # Scheduler + task management
│   ├── sem.c
│   ├── mutex.c
│   ├── queue.c
│   ├── timer.c
│   ├── list.c        # Internal sorted linked list
│   └── compat.h      # Compiler compatibility (MSVC / GCC / Clang)
├── port/
│   ├── arm_cm0plus/       # SysTick, PendSV context switch (RP2040)
│   ├── arm_cm4/           # SysTick, BASEPRI critical sections, PendSV (generic M4F)
│   ├── avr_atmega/        # Timer1 CTC, cli/sei, ISR_NAKED context switch (ATmega328P)
│   ├── riscv/             # CLINT timer, machine-mode trap handler (RV32IMAC)
│   ├── xtensa_esp32s3/    # TIMG0 timer, interrupt matrix, Xtensa level-1 ISR (ESP32-S3)
│   └── host/              # POSIX ucontext simulation port (macOS / Linux)
├── samples/
│   ├── blink.c                  # Single task + delay (LED blink)
│   ├── producer_consumer.c      # Queue: producer sends, consumer receives
│   ├── mutex_shared_resource.c  # Mutex: two tasks sharing a counter
│   ├── uart_echo.c              # ISR→task decoupling via ring buffer + task notification
│   ├── adc_pipeline.c           # 100 Hz ADC sampling pipeline with queue + rolling average
│   ├── wifi_http.c              # Pico W: WiFi connect + HTTP GET (semaphore sequencing)
│   └── lwipopts.h               # Minimal lwIP config for wifi_http
├── cmake/
│   ├── avr_atmega328p.cmake   # avr-gcc toolchain file
│   ├── riscv32_clint.cmake    # riscv64-unknown-elf-gcc toolchain file (rv32imac)
│   ├── esp32s3_qemu.cmake     # xtensa-esp32s3-elf-gcc toolchain file (Call0 ABI)
│   └── arm_cm4.cmake          # arm-none-eabi-gcc toolchain file (Cortex-M4F)
├── test/
│   ├── test.h        # Testy unit-test framework (vendored)
│   ├── test_main.c   # Testy entry point
│   └── test_rtos.c   # Host-side unit tests (44 suites, 318 assertions)
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

### Debug and instrumentation knobs

| Macro | Default | Meaning |
|---|---|---|
| `RTOS_STACK_OVERFLOW_CHECK` | 1 | Sentinel fill + per-tick check for running task + full check in idle |
| `RTOS_STACK_WATERMARK` | 0 | Fill entire stack on create; `rtos_task_stack_high_water_mark()` counts untouched words |
| `RTOS_ENABLE_TRACE` | 0 | Enable `RTOS_TRACE_*` hook macros (see `include/rtos_trace.h`) |
| `RTOS_ENABLE_RUNTIME_STATS` | 0 | Per-task tick counter + `rtos_task_get_runtime_stats()` |

### Power / tickless knobs

| Macro | Default | Meaning |
|---|---|---|
| `RTOS_TICKLESS_IDLE` | 0 | Skip ticks while all tasks are blocked; requires `port_suppress_ticks()` |
| `RTOS_IDLE_HOOK_FUNCTION` | (none) | `void fn(void)` called from idle on every idle loop iteration |

### Multi-core knobs

| Macro | Default | Meaning |
|---|---|---|
| `RTOS_NUM_CORES` | 1 | `1` = single-core; `2` = AMP dual-core (RP2040 / ESP32-S3) |

---

## Building

### Host unit tests (macOS / Linux / Windows)

```sh
cmake -B build
cmake --build build
./build/rtos_test          # macOS / Linux
# or
.\build\Debug\rtos_test.exe  # Windows (MSVC)
```

The test suite uses the [testy](https://github.com/mseminatore/testy) micro-framework
(vendored in `test/test.h`).  It builds on GCC, Clang, and MSVC without extra
dependencies.

### Selecting a port

Each cross-compile preset is selected by its CMake toolchain file (the toolchain file
sets `CMAKE_SYSTEM_PROCESSOR` so the build picks the right port sources). You can also
force the port explicitly with `-DRTOS_PORT=<name>` (overrides the auto-detected value):

```sh
cmake -B build -DRTOS_PORT=host           # host unit tests only (no port sources)
cmake -B build_avr     -DRTOS_PORT=avr     -DCMAKE_TOOLCHAIN_FILE=cmake/avr_atmega328p.cmake
cmake -B build_rv32    -DRTOS_PORT=riscv   -DCMAKE_TOOLCHAIN_FILE=cmake/riscv32_clint.cmake
cmake -B build_esp32s3 -DRTOS_PORT=esp32s3 -DCMAKE_TOOLCHAIN_FILE=cmake/esp32s3_qemu.cmake
cmake -B build_cm4     -DRTOS_PORT=cm4     -DCMAKE_TOOLCHAIN_FILE=cmake/arm_cm4.cmake
```

Valid `RTOS_PORT` values: `pico`, `avr`, `riscv`, `esp32s3`, `cm4`, `host`.

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

// AMP: pin a task to a specific core regardless of which core calls this.
// Only available when RTOS_NUM_CORES > 1.
rtos_handle_t h = rtos_task_create_on_core(&my_tcb, my_stack, 256,
                                            my_task_func, NULL, "myTask", 3,
                                            1 /* core */);

void rtos_task_delay(uint32_t ticks);   // block for N ticks

// Drift-free periodic delay. Initialise *last_wake to rtos_task_tick_count()
// before the loop, then call on each iteration. Advances *last_wake by period
// each call, delaying only the remaining time until the next deadline.
void rtos_task_delay_until(uint32_t *last_wake_tick, uint32_t period_ticks);

void rtos_task_yield(void);
void rtos_task_suspend(rtos_handle_t);
void rtos_task_resume(rtos_handle_t);
void rtos_task_delete(rtos_handle_t);   // pass NULL for current task

// Task notifications — lightweight per-task signal with optional 32-bit
// value. Common pattern for ISR→task or task→task signaling, zero extra
// allocation. Includes binary, set-bits, counting, and overwrite semantics.
void rtos_task_notify(rtos_handle_t task);            // from task context
void rtos_task_notify_from_isr(rtos_handle_t task);  // from ISR context
int  rtos_task_notify_wait(uint32_t timeout_ticks);  // RTOS_OK or RTOS_TIMEOUT
void rtos_task_notify_clear(void);   // discard a pending notification without waiting

// Value-passing notifications (FreeRTOS-style):
//   action ∈ { RTOS_NOTIFY_NONE,           // just mark pending
//              RTOS_NOTIFY_SET_BITS,       // value |= bits
//              RTOS_NOTIFY_INCREMENT,      // value++ (counting)
//              RTOS_NOTIFY_OVERWRITE,      // value = bits
//              RTOS_NOTIFY_SET_NO_OVERWRITE } // fails if already pending
int  rtos_task_notify_value(rtos_handle_t task, rtos_notify_action_t action, uint32_t value);
int  rtos_task_notify_value_from_isr(rtos_handle_t task, rtos_notify_action_t action, uint32_t value);
int  rtos_task_notify_wait_value(uint32_t clear_on_entry, uint32_t clear_on_exit,
                                 uint32_t *value_out, uint32_t timeout_ticks);

// Inspection and priority control
rtos_task_state_t rtos_task_get_state(rtos_handle_t task);    // TASK_READY etc.
const char       *rtos_task_get_name(rtos_handle_t task);
uint8_t           rtos_task_get_priority(rtos_handle_t task); // effective priority
int               rtos_task_set_priority(rtos_handle_t task, uint8_t prio);
                                      // prio must be 0 .. RTOS_MAX_PRIORITIES-2
                                      // returns RTOS_OK or RTOS_ERR; pass NULL for self

// Debug (compile with RTOS_STACK_OVERFLOW_CHECK / RTOS_STACK_WATERMARK)
int      rtos_task_check_stack(rtos_handle_t);             // RTOS_OK or RTOS_ERR
uint32_t rtos_task_stack_high_water_mark(rtos_handle_t);   // words never written

// AMP: pass to multicore_launch_core1() to start core 1's scheduler.
// Only available when RTOS_NUM_CORES > 1.
void rtos_core1_entry(void);
```

### Semaphores
```c
static rtos_sem_t my_sem;
rtos_handle_t s = rtos_semaphore_create_binary(&my_sem);
// or:
rtos_handle_t s = rtos_semaphore_create_counting(&my_sem, max, initial);

rtos_semaphore_give(s);
rtos_semaphore_take(s, RTOS_WAIT_FOREVER);   // returns RTOS_OK or RTOS_TIMEOUT
rtos_semaphore_give_from_isr(s);             // ISR-safe give; does not reschedule
int rtos_semaphore_take_from_isr(s);         // ISR-safe try-take; RTOS_OK or RTOS_ERR
```

### Mutexes

Mutexes include **priority inheritance with full multi-mutex tracking**: when a
high-priority task blocks waiting for a mutex held by a lower-priority task, the owner's
priority is temporarily boosted to the highest waiter's level so a medium-priority task
cannot starve the owner.  When the owner holds several mutexes, its boost is recomputed
across all held mutexes on every lock/unlock/timeout, so a boost from one mutex is
preserved while another is released.  Note: this is **single-level** inheritance — boost
does not propagate transitively across chains of waiters.

```c
static rtos_mutex_t my_mutex;
rtos_handle_t m = rtos_mutex_create(&my_mutex);

rtos_mutex_lock(m, RTOS_WAIT_FOREVER);
rtos_mutex_unlock(m);   // returns RTOS_OK, or RTOS_ERR if caller is not the owner

// Recursive variant: the same task may lock multiple times; must unlock the
// same number of times before another task can acquire.
static rtos_mutex_t rec_mutex;
rtos_handle_t rm = rtos_mutex_create_recursive(&rec_mutex);
rtos_mutex_lock(rm, RTOS_WAIT_FOREVER);   // nest_count = 1
rtos_mutex_lock(rm, RTOS_WAIT_FOREVER);   // nest_count = 2
rtos_mutex_unlock(rm);                    // nest_count = 1
rtos_mutex_unlock(rm);                    // nest_count = 0; released
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
rtos_queue_spaces_available(q);

// Peek at the next item without removing it.
rtos_queue_peek(q, &val, RTOS_NO_WAIT);

// Send to the front of the queue (LIFO) for high-priority messages.
rtos_queue_send_to_front(q, &val, RTOS_WAIT_FOREVER);
```

### Software timers
```c
static rtos_timer_t my_timer;
rtos_handle_t t = rtos_timer_create(&my_timer, "blink", 500, /*periodic=*/1, blink_cb);
rtos_timer_start(t);   // fails silently if RTOS_MAX_TIMERS active timers already exist
rtos_timer_stop(t);
rtos_timer_reset(t);   // restart countdown from full period (starts if not active)
int running = rtos_timer_is_active(t);  // 1 if active, 0 otherwise
```

Timer callbacks are invoked from the tick ISR (normal mode) or from the idle task (tickless
idle mode).  Callbacks must not call `rtos_timer_reset()` on the timer that is currently
firing.  Calling `rtos_timer_stop()` on the currently-firing timer from its own callback
is supported.

Software timers use an **O(k)** sorted-list approach: the active timer list is kept sorted
by absolute expiry tick so the tick handler only inspects the head, not all timers.

---

## Debug and instrumentation

### Stack overflow detection

Stack overflow detection is **on by default** (`RTOS_STACK_OVERFLOW_CHECK=1`). It writes four
`0xDEADBEEF` sentinel words at the bottom of every task stack at creation time, and checks
them every tick for the currently-running task (fast) plus on every idle-task iteration for
all tasks (thorough).

When corruption is detected the weak `rtos_stack_overflow_hook` is called (spins by default);
override it in your application to log the task name and halt:

```c
void rtos_stack_overflow_hook(rtos_tcb_t *tcb)
{
    printf("STACK OVERFLOW: %s\n", tcb->name);
    for (;;);
}
```

**Stack high-water mark** (`RTOS_STACK_WATERMARK=1`): fills the entire stack with `0xA5A5A5A5`
at creation, then lets you query how close to full a task's stack has grown:

```c
uint32_t free_words = rtos_task_stack_high_water_mark(task_handle);
```

### Trace hooks

Compile with `RTOS_ENABLE_TRACE=1` to activate the trace macros in `include/rtos_trace.h`.
All hooks are zero-cost when disabled (expand to `((void)0)`).

Implement any of the `rtos_trace_*` weak functions to receive events:

```c
void rtos_trace_task_switched_in(rtos_tcb_t *tcb)
{
    // e.g. write a timestamp + task name to a ring buffer
    // or call SEGGER_SYSVIEW_OnTaskStartExec(...)
}
```

Available hooks: `task_switched_in/out`, `task_create/delete`, `sem_take/give`,
`mutex_lock/unlock`, `queue_send/receive`, `timer_fire`.

### Runtime CPU statistics

Compile with `RTOS_ENABLE_RUNTIME_STATS=1` to track per-task CPU tick usage:

```c
rtos_runtime_stat_t stats[RTOS_MAX_TASKS];
size_t n = rtos_task_get_runtime_stats(stats, RTOS_MAX_TASKS);
for (size_t i = 0; i < n; i++)
    printf("%-16s  %6lu ticks  %3u%%\n",
           stats[i].name, (unsigned long)stats[i].runtime_ticks, stats[i].percent);
```

---

## Low-power / tickless idle

By default the idle task calls `port_cpu_idle()` (WFI on ARM/RISC-V, WAITI on Xtensa)
which sleeps until the next tick interrupt.  This saves power with no configuration required.

### Optional idle hook

Define `RTOS_IDLE_HOOK_FUNCTION` in `rtos_config.h` to call your own code from the idle
task on every idle iteration (e.g. to feed a watchdog, blink an LED, or gather diagnostics):

```c
#define RTOS_IDLE_HOOK_FUNCTION  my_idle_hook
void my_idle_hook(void) { /* feed watchdog, etc. */ }
```

### Tickless idle

Set `RTOS_TICKLESS_IDLE=1` to suppress tick interrupts entirely when all tasks are
blocked on delays.  The scheduler calculates the soonest wakeup, reprograms the tick
timer for that duration, executes WFI, then credits the elapsed ticks on wake-up.
This can dramatically reduce idle current on battery-powered devices.

On the **RP2040** (`port/arm_cm0plus/`), `port_suppress_ticks()` reprograms SysTick
for up to 24-bit counts worth of ticks, issues WFI, measures elapsed cycles, then
restores the normal period.

The host simulation port (`port/host/`) implements tickless via `usleep()` and is
useful for verifying tickless logic in tests.

---

## AMP multi-core (RP2040 / ESP32-S3)

Set `RTOS_NUM_CORES=2` for **Asymmetric Multi-Processing (AMP)**: each CPU core runs
an independent scheduler instance with its own ready/blocked lists, idle task, and
tick counter.  Cores communicate via shared queues protected by cross-core critical
sections.

### How it works

- All scheduler globals become `[RTOS_NUM_CORES]` arrays indexed by `port_core_id()`.
- **Critical sections** (`RTOS_NUM_CORES > 1`): first disable IRQs (`cpsid i`), then
  claim **SIO spinlock 0** (RP2040) so the other core cannot enter simultaneously.
  Release order is reversed: drop spinlock, re-enable IRQs.
- Use `rtos_task_create_on_core(tcb, stack, words, fn, arg, name, prio, core)` to pin
  a task to a specific core — callable from either core, from `main()` before the
  scheduler starts. `rtos_task_create()` pins to the calling core.
- There is **no task migration** — a task stays on its home core for its lifetime.
  Use `rtos_queue_send` / `rtos_queue_receive` across cores for inter-core data.

### RP2040 startup pattern

Pin all tasks to their cores from `main()` using `rtos_task_create_on_core()`, then
pass the built-in `rtos_core1_entry` to `multicore_launch_core1()` before calling
`rtos_start()`. `rtos_core1_entry` configures core 1's SysTick, creates its idle task,
and starts its scheduler — no user-written entry wrapper is needed.

```c
#include "rtos.h"
#include "pico/multicore.h"   // Raspberry Pi Pico SDK

static rtos_tcb_t   c0_tcb, c1_tcb;
static uint32_t     c0_stack[256], c1_stack[256];
static rtos_queue_t shared_queue;
static uint8_t      shared_buf[4 * sizeof(uint32_t)];
rtos_handle_t       g_queue;

static void core1_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t val;
        if (rtos_queue_receive(g_queue, &val, RTOS_WAIT_FOREVER) == RTOS_OK)
            printf("core1 got %lu\n", (unsigned long)val);
    }
}

static void core0_task(void *arg)
{
    (void)arg;
    uint32_t n = 0;
    for (;;) {
        rtos_queue_send(g_queue, &n, RTOS_WAIT_FOREVER);
        n++;
        rtos_task_delay(1000);
    }
}

int main(void)
{
    // Create shared queue before any task runs
    g_queue = rtos_queue_create(&shared_queue, shared_buf, sizeof(uint32_t), 4);

    // Pin tasks to their cores — callable from main() before either scheduler starts
    rtos_task_create_on_core(&c0_tcb, c0_stack, 256, core0_task, NULL, "c0", 1, 0);
    rtos_task_create_on_core(&c1_tcb, c1_stack, 256, core1_task, NULL, "c1", 1, 1);

    // Start core 1's scheduler, then core 0's (rtos_start never returns)
    multicore_launch_core1(rtos_core1_entry);
    rtos_start();
}
```

### AMP guidelines

- **Shared objects** (queues, semaphores, mutexes) must reside in shared RAM (default
  on RP2040 — all RAM is shared; on ESP32-S3 avoid DRAM0/1 if they differ per core).
- **Do not share TCBs or stacks** between cores.  Each core owns its tasks entirely.
- **No priority inheritance across cores** — a high-priority task on core 1 does not
  preempt a lower-priority task on core 0.
- **Tick counts are independent** per core; do not use `rtos_task_tick_count()` for
  cross-core time synchronisation.

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

### General samples (host + RP2040)

Six demos in `samples/`. They build and run on the host (macOS / Linux) via
the `port/host/` simulation port; on RP2040 the hardware stubs are replaced
with real GPIO/UART/ADC calls via `#ifdef __rp2040__`.

| Binary | Concepts demonstrated |
|---|---|
| `sample_blink` | Single task, `rtos_task_delay_until` for drift-free 500 ms LED blink |
| `sample_producer_consumer` | Queue: producer sends integers, consumer prints them |
| `sample_mutex_shared_resource` | Mutex: two tasks increment a shared counter safely |
| `sample_uart_echo` | ISR → task decoupling: UART RX ISR fills ring buffer, notifies task via `rtos_task_notify_from_isr` |
| `sample_adc_pipeline` | Sampling pipeline: 100 Hz sampler (`rtos_task_delay_until`) → queue → rolling-average monitor task |

```sh
cmake -B build && cmake --build build
./build/sample_blink
./build/sample_producer_consumer
./build/sample_mutex_shared_resource
./build/sample_uart_echo          # type characters; press Enter to see a line echoed
./build/sample_adc_pipeline       # prints simulated temperature readings every second
```

### Pico W–specific samples

Two additional samples target RP2040 hardware; `wifi_http` requires a Pico W
(CYW43 WiFi chip).

| Binary | Concepts demonstrated |
|---|---|
| `sample_uart_echo` | Same as above, but wired to UART0 (GP0/GP1, 115200 baud) |
| `sample_adc_pipeline` | Same as above, reading the RP2040 internal temperature sensor (ADC ch 4) |
| `sample_wifi_http` | Binary semaphore sequences WiFi-connect task → HTTP-client task; LED blink task runs concurrently |

```sh
export PICO_SDK_PATH=~/pico-sdk

# Pico / Pico W — uart_echo and adc_pipeline
cmake -B build_pico -DPICO_BOARD=pico_w
cmake --build build_pico
# Flash build_pico/sample_uart_echo.uf2 or sample_adc_pipeline.uf2

# Pico W — WiFi HTTP client (supply your network credentials)
cmake -B build_picow \
      -DPICO_BOARD=pico_w \
      -DWIFI_SSID="YourNetwork" \
      -DWIFI_PASSWORD="YourPassword"
cmake --build build_picow
# Flash build_picow/sample_wifi_http.uf2
# Open USB serial; the device prints its public IP every 30 seconds
```

`wifi_http` requires `lwipopts.h` (provided in `samples/`) which configures
lwIP for the Pico W background-IRQ integration.

---



## Porting to a new architecture

1. Copy `port/arm_cm0plus/` (or `port/riscv/`) to `port/<your-arch>/`
2. Implement `port.c`: `port_init`, `port_enter_critical`, `port_exit_critical`, `port_request_reschedule`, `port_start_first_task`, `port_init_stack`, `port_cpu_idle`, `port_core_id`
3. Implement `port_asm.S` (or use inline asm in `port.c`): the context-switch handler
4. Optionally implement `port_suppress_ticks(max_ticks)` for tickless idle support
5. Add a CMake toolchain file in `cmake/` and update `CMakeLists.txt` to select the port sources

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

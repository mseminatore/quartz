# Quartz RTOS

*A small, fast, portable, real-time operating system written in C.*

> The name Quartz was chosen because it suggests many of the project design 
> goals. Specifically, well-known attributes of Quartz crystals are that they 
> are timely, accurate, precise, and solid.

## Why Quartz RTOS

For fun and for learning, I build many small projects using microcontrollers 
ranging from 8-bit like PIC and AVR, up to 32-bit processors like the ARM-M0+ 
(RPi PICO) and Xtensa L7 (ESP32-S3).

While programming small projects to bare metal is usually adequate, there are 
times when the the support of a minimal operating system would be useful. For
example, when multiple tasks and/or multiple cores are needed.

I started by evaluating FreeRTOS and Zephyr. Both are highly capable, well 
supported and mature. Still, they both seemed like more than I needed. 
Having previously written a multi-tasking kernel in assembly language as part of the 
[bintools](https://github.com/mseminatore/bintools) project I decided to create
my own.

New ports may be added as needed. The most likely next candidate is the RP2350 
(ARM Cortex-M33) for the Raspberry Pi PICO 2. If there is a processor you'd like
supported, let us know, and please consider contributing.

**Design goals:**
- Preemptive, priority-based scheduling (round-robin within equal priorities)
- Static memory allocation only — no `malloc`, no kernel allocations or surprises
- Easily portable — architecture-specific code isolated in `port/<arch>/`
- Host-testable kernel logic (unit tests run on the development machine)
- O(k) tick handler — only examines the *k* tasks expiring on the current tick, not all blocked tasks
- Priority-inheritance mutex — prevents unbounded priority inversion (single-level boost)

**Currently supported targets:** 

- RP2040 (ARM Cortex-M0+)
- ARM Cortex-M4F (generic bare-metal, e.g. STM32F4xx)
- AVR ATmega328P (Arduino Uno/Nano)
- RISC-V RV32IMAC (QEMU virt / SiFive FE310)
- ESP32-S3 (Xtensa LX7, Espressif QEMU)

## Build Status

| Family       | CI status |
|--------------|-----------|
| Host         | [![host-linux](https://github.com/mseminatore/quartz/actions/workflows/host-linux.yml/badge.svg)](https://github.com/mseminatore/quartz/actions/workflows/host-linux.yml) [![host-windows](https://github.com/mseminatore/quartz/actions/workflows/host-windows.yml/badge.svg)](https://github.com/mseminatore/quartz/actions/workflows/host-windows.yml) |
| ARM Cortex-M | [![cm0plus](https://github.com/mseminatore/quartz/actions/workflows/cm0plus.yml/badge.svg)](https://github.com/mseminatore/quartz/actions/workflows/cm0plus.yml) [![cm3](https://github.com/mseminatore/quartz/actions/workflows/cm3.yml/badge.svg)](https://github.com/mseminatore/quartz/actions/workflows/cm3.yml) [![cm4](https://github.com/mseminatore/quartz/actions/workflows/cm4.yml/badge.svg)](https://github.com/mseminatore/quartz/actions/workflows/cm4.yml) [![cm4-fpu](https://github.com/mseminatore/quartz/actions/workflows/cm4-fpu.yml/badge.svg)](https://github.com/mseminatore/quartz/actions/workflows/cm4-fpu.yml) [![cm7](https://github.com/mseminatore/quartz/actions/workflows/cm7.yml/badge.svg)](https://github.com/mseminatore/quartz/actions/workflows/cm7.yml) |
| AVR          | [![avr](https://github.com/mseminatore/quartz/actions/workflows/avr.yml/badge.svg)](https://github.com/mseminatore/quartz/actions/workflows/avr.yml) |
| RISC-V       | [![riscv](https://github.com/mseminatore/quartz/actions/workflows/riscv.yml/badge.svg)](https://github.com/mseminatore/quartz/actions/workflows/riscv.yml) |
| Tracing      | [![trace](https://github.com/mseminatore/quartz/actions/workflows/trace.yml/badge.svg)](https://github.com/mseminatore/quartz/actions/workflows/trace.yml) |

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

### Debug and instrumentation settings

| Macro | Default | Meaning |
|---|---|---|
| `RTOS_STACK_OVERFLOW_CHECK` | 1 | Sentinel fill + per-tick check for running task + full check in idle |
| `RTOS_STACK_WATERMARK` | 0 | Fill entire stack on create; `rtos_task_stack_high_water_mark()` counts untouched words |
| `RTOS_ENABLE_TRACE` | 0 | Enable `RTOS_TRACE_*` hook macros (see `include/rtos_trace.h`) |
| `RTOS_ENABLE_RUNTIME_STATS` | 0 | Per-task tick counter + `rtos_task_get_runtime_stats()` |

### Power / tickless settings

| Macro | Default | Meaning |
|---|---|---|
| `RTOS_TICKLESS_IDLE` | 0 | Skip ticks while all tasks are blocked; requires `port_suppress_ticks()` |
| `RTOS_IDLE_HOOK_FUNCTION` | (none) | `void fn(void)` called from idle on every idle loop iteration |

### Multi-core settings

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
(git submodule in `extern/testy`).  It builds on GCC, Clang, and MSVC without extra
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
cmake -B build_cm0plus -DRTOS_PORT=cm0plus -DCMAKE_TOOLCHAIN_FILE=cmake/arm_cm0plus_generic.cmake
cmake -B build_cm3     -DRTOS_PORT=cm3     -DCMAKE_TOOLCHAIN_FILE=cmake/arm_cm3.cmake
cmake -B build_cm4     -DRTOS_PORT=cm4     -DCMAKE_TOOLCHAIN_FILE=cmake/arm_cm4.cmake
cmake -B build_cm7     -DRTOS_PORT=cm7     -DCMAKE_TOOLCHAIN_FILE=cmake/arm_cm7.cmake
```

Valid `RTOS_PORT` values: `pico`, `cm0plus`, `cm3`, `cm4`, `cm7`, `avr`, `riscv`, `esp32s3`, `host`.

`cm0plus` is the bare-metal Cortex-M0+ flavour (no SDK). Use `pico` if you want
the Pico SDK pulled in for RP2040 boards. `cm3`, `cm4`, and `cm7` all share the
same `port/arm_cm4/` sources (Thumb-2 ISA, BASEPRI, PendSV) — only the
toolchain `-mcpu`/`-mfpu` flags differ.

See [PORTS.md](PORTS.md) for more details.

---

## Quartz API

### Kernel
```c
void     rtos_start(void);              // start scheduler — never returns!
rtos_tick_t rtos_task_tick_count(void);
```

### Tasks
```c
// All storage (TCB + stack) must be static/global — provided by the caller.
static rtos_tcb_t   my_tcb;
static rtos_stack_t my_stack[256];

rtos_handle_t h = rtos_task_create(&my_tcb, my_stack, 256,
                                   my_task_func, NULL, "myTask", 3);

// AMP: pin a task to a specific core regardless of which core calls this.
// Only available when RTOS_NUM_CORES > 1.
rtos_handle_t h = rtos_task_create_on_core(&my_tcb, my_stack, 256,
                                            my_task_func, NULL, "myTask", 3,
                                            1 /* core */);

void rtos_task_delay(rtos_tick_t ticks);   // block for N ticks

// Drift-free periodic delay. Initialise *last_wake to rtos_task_tick_count()
// before the loop, then call on each iteration. Advances *last_wake by period
// each call, delaying only the remaining time until the next deadline.
void rtos_task_delay_until(rtos_tick_t *last_wake_tick, rtos_tick_t period_ticks);

void rtos_task_yield(void);
void rtos_task_suspend(rtos_handle_t);
void rtos_task_resume(rtos_handle_t);
void rtos_task_delete(rtos_handle_t);   // pass NULL for current task

// Task notifications — lightweight per-task signal with optional 32-bit
// value. Common pattern for ISR→task or task→task signaling, zero extra
// allocation. Includes binary, set-bits, counting, and overwrite semantics.
void rtos_task_notify(rtos_handle_t task);            // from task context
void rtos_task_notify_from_isr(rtos_handle_t task);  // from ISR context
int  rtos_task_notify_wait(rtos_tick_t timeout_ticks);  // RTOS_OK or RTOS_TIMEOUT
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
                                 uint32_t *value_out, rtos_tick_t timeout_ticks);

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

## Tracing & visualization

The kernel ships with a built-in trace recorder that captures every scheduler
event (task switch in/out, create/delete) plus IPC events (sem take/give,
mutex lock/unlock, queue send/receive, timer fire) into a static ring buffer.
The capture can be exported to **Chrome Trace Event Format** and opened in
[`chrome://tracing`](chrome://tracing) or [Perfetto](https://ui.perfetto.dev)
for a SystemView-style visual timeline.

Enable the recorder by selecting the chrome backend at configure time.

> **Platform note** — `sample_trace_demo` runs on the host POSIX simulator
> (`port/host/port.c`), which uses `<ucontext.h>`. That header is **not
> available with native MSVC**, so the demo only builds on Linux, macOS,
> or Windows Subsystem for Linux (WSL). The kernel itself, the unit tests
> (`rtos_test`), and every cross-compile target build fine on native
> Windows — only the host samples are POSIX-only.

### Linux / macOS

```sh
cmake -B build -DRTOS_TRACE_BACKEND=chrome
cmake --build build --target sample_trace_demo
./build/sample_trace_demo > capture.hex
python3 tools/trace_to_chrome.py capture.hex --hex -o trace.json
# Then open trace.json in chrome://tracing or https://ui.perfetto.dev
```

### Windows (via WSL)

From a PowerShell or `cmd` prompt at the repo root:

```powershell
wsl -e bash -lc "cd /mnt/c/dev/quartz && cmake -B build_wsl_trace -DRTOS_TRACE_BACKEND=chrome"
wsl -e bash -lc "cd /mnt/c/dev/quartz && cmake --build build_wsl_trace --target sample_trace_demo"
wsl -e bash -lc "cd /mnt/c/dev/quartz && ./build_wsl_trace/sample_trace_demo > capture.hex"
wsl -e bash -lc "cd /mnt/c/dev/quartz && python3 tools/trace_to_chrome.py capture.hex --hex -o trace.json"
```

Then open `trace.json` (which now lives in your repo on the Windows side)
in `chrome://tracing` or [Perfetto UI](https://ui.perfetto.dev).

> Use a **separate build directory** (e.g. `build_wsl_trace/`) for the WSL
> build so it doesn't collide with your native MSVC `build/` directory.

### How to read the visualization

Each task gets **two adjacent lanes**: a `running` lane and an `IPC`
sub-lane immediately under it.

| Lane                              | What it shows                                                       |
|-----------------------------------|---------------------------------------------------------------------|
| `producer`, `consumer`, `idle`, … | One row per task (the `name` you passed to `rtos_task_create`). A coloured bar named **`running`** spans every interval that this task was on-CPU. Click a bar to see its priority. |
| `producer/IPC`, `consumer/IPC`, … | IPC bars (`sem_take`, `sem_give`, `mutex_lock`, `mutex_unlock`, `queue_send`, `queue_recv`, `timer_fire`) attributed to whichever task was running when the call happened. Each bar carries event-specific args (see below). |
| `events`                          | Lazily added; holds any IPC event that occurred before the first task switch (rare). |

IPC bars have a target width of 50 µs so they remain selectable in both
`chrome://tracing` and Perfetto. The decoder also:
- nudges truly simultaneous events forward by 1 µs each so bars never stack;
- shrinks a bar when the next event on the same sub-lane is closer (floor 1 µs);
- equalizes widths within a tight cluster and re-spaces members at uniform
  ~20 µs slots, so all events in a producer-style burst look the same size and
  remain clickable (otherwise simultaneous-µs events collapse to invisible
  1 µs slivers);
- color-codes each event by type (`mutex_lock` red, `mutex_unlock` yellow,
  `queue_send` lavender, `queue_recv` orange, `sem_take` green, `sem_give`
  olive, `timer_fire` white) so closely-spaced bars are still distinguishable.

The **start timestamp** is the exact microsecond the kernel call completed
(within ±1 µs after the simultaneous-event nudge); the width is purely visual
and **does not** represent how long the call took.

Click an IPC bar to inspect its `args`:

| Event          | `args` keys                                |
|----------------|--------------------------------------------|
| `sem_take`     | `count` (after take), `blocked` (1 if waited) |
| `sem_give`     | `count` (after give), `woke_waiter`        |
| `mutex_lock`   | `nest_count`, `blocked`                    |
| `mutex_unlock` | `nest_count` (after unlock), `woke_waiter` |
| `queue_send`   | `count` (after send), `woke_receiver`      |
| `queue_recv`   | `count` (after recv), `woke_sender`        |

Reading the layout:

- **Gaps** in a task's `running` row = the task was blocked or pre-empted.
- A bar in `idle/running` = no other task was ready (the system is idle).
- A bar appearing in one task's `running` immediately followed by another
  = a context switch; the timestamps are exact (microsecond resolution).
- Use **W**/**S** in `chrome://tracing` to zoom, **A**/**D** to pan.

### Capturing a chrome trace from a Pico W via the Debug Probe

The same recorder + decoder pipeline works on real hardware — the only
extra step is shuttling the hex dump back to the host over a serial link.
The bundled sample `samples/pico_trace_demo.c` (built only when the chrome
backend is selected) does exactly that: it runs producer/consumer for 20
iterations, then prints the trace ring buffer over USB CDC framed by
`---BEGIN-TRACE---` / `---END---` markers.

1. **Build with the chrome backend** (any RP2040 board; substitute
   `pico` for `pico_w` if you're on the non-wireless Pico):

   ```sh
   export PICO_SDK_PATH=~/pico-sdk
   cmake -B build_pico -DPICO_BOARD=pico_w -DRTOS_TRACE_BACKEND=chrome
   cmake --build build_pico --target sample_pico_trace_demo
   ```

2. **Flash the .uf2 via the Pi Debug Probe** (uses the probe's SWD pins,
   no need to enter BOOTSEL):

   ```sh
   picotool load -fx build_pico/sample_pico_trace_demo.uf2
   # or, if you prefer openocd:
   openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg \
           -c "program build_pico/sample_pico_trace_demo.elf verify reset exit"
   ```

3. **Capture the serial output** from the Pico's native USB port (the
   debug probe's UART bridge is not used in this flow — the probe handles
   only flashing/SWD). The Pico enumerates as `/dev/ttyACM0` on Linux/WSL
   or as a COM port on Windows:

   ```sh
   # Linux / macOS / WSL2:
   cat /dev/ttyACM0 > capture.txt &
   sleep 15 && pkill cat
   # Windows: open the COM port in PuTTY / Tera Term with logging enabled,
   #          let the demo run for ~10 s, then save the log as capture.txt.
   ```

4. **Decode and view** — the python decoder strips all non-hex characters
   and stops at `---END---`, so you can feed it the raw serial log without
   manually trimming the printf chatter:

   ```sh
   python3 tools/trace_to_chrome.py capture.txt --hex -o trace.json
   ```

   Open `trace.json` in `chrome://tracing` or [Perfetto UI](https://ui.perfetto.dev).

> **Tip** — to capture from the probe's UART bridge (`/dev/ttyACM1` on a
> Linux host with both probe and Pico USB connected) instead of the Pico's
> own USB, edit the `pico_enable_stdio_usb` / `pico_enable_stdio_uart`
> calls in `CMakeLists.txt` for `sample_pico_trace_demo`. The default uses
> the Pico's USB CDC because it works whether or not a probe is attached.

settings (set via `-D` at configure time, or in `rtos_config.h`):

| Macro                        | Default | Purpose                                          |
|------------------------------|---------|--------------------------------------------------|
| `RTOS_TRACE_BACKEND`         | `none`  | `none` / `chrome` / `sysview` (Phase 3 reserved) |
| `RTOS_TRACE_BUFFER_BYTES`    | 4096    | Ring buffer size — 16 bytes per event            |
| `RTOS_TRACE_HANDLE_TABLE_SIZE` | 32    | Distinct named objects (tasks + sem + mutex + queue) |

Each event is a fixed 16-byte record (`include/rtos_trace_chrome.h`); the
recorder adds **a few hundred cycles** of overhead per event on Cortex-M4
when the backend is enabled.  Application code drains the buffer with
`rtos_trace_chrome_dump_hex()` (e.g. over UART) or `rtos_trace_chrome_serialize()`
(for binary transports such as RTT or USB).

Every port implements `port_timestamp_us()`, which returns a monotonic
microsecond counter using the best source available on that target
(DWT.CYCCNT on Cortex-M3/M4/M7, CLINT MTIME on RISC-V, CLOCK_MONOTONIC on
host, SysTick + tick interpolation on Cortex-M0+, Timer1 sub-tick on AVR,
TIMG0 1 MHz counter on ESP32-S3).

A future `RTOS_TRACE_BACKEND=sysview` option will write SEGGER SystemView
records over RTT for live streaming into the SystemView desktop app.

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
static rtos_stack_t c0_stack[256], c1_stack[256];
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
static rtos_stack_t task1_stack[256], task2_stack[256];
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

Five demos in `samples/`. They build and run on the host (macOS / Linux) via
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

### Tracing samples (chrome backend)

Two extra samples are built **only** when the configure flag
`-DRTOS_TRACE_BACKEND=chrome` is set. They drive the kernel through enough
events to fill the trace ring buffer, then dump it as ASCII hex for the
`tools/trace_to_chrome.py` decoder. See [Tracing & visualization](#tracing--visualization)
for the full build / capture / decode / view recipe.

| Binary | Target | Concepts demonstrated |
|---|---|---|
| `sample_trace_demo` | host (POSIX — Linux / macOS / WSL; not native MSVC) | Producer + consumer exercising sem / mutex / queue / context-switch trace events; dumps to stdout |
| `sample_pico_trace_demo` | RP2040 / Pico / Pico W | Same workload on real hardware; dumps over USB CDC framed by `---BEGIN-TRACE---` / `---END---` markers, captured via the Pi Debug Probe or BOOTSEL flash |

```sh
# Host (Linux / macOS / WSL)
cmake -B build_trace -DRTOS_TRACE_BACKEND=chrome
cmake --build build_trace --target sample_trace_demo

# Pico / Pico W
export PICO_SDK_PATH=~/pico-sdk
cmake -B build_pico_trace -DPICO_BOARD=pico_w -DRTOS_TRACE_BACKEND=chrome
cmake --build build_pico_trace --target sample_pico_trace_demo
```

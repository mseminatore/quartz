# Copilot Instructions

## Build & Test

```sh
# Host unit tests (default — no hardware needed)
cmake -B build && cmake --build build
./build/rtos_test          # run all tests
ctest --test-dir build     # via CTest

# Cross-compile targets (no unit tests, hardware/QEMU only)
cmake -B build_avr   -DCMAKE_TOOLCHAIN_FILE=cmake/avr_atmega328p.cmake && cmake --build build_avr
cmake -B build_rv32  -DCMAKE_TOOLCHAIN_FILE=cmake/riscv32_clint.cmake  && cmake --build build_rv32
cmake -B build_esp32s3 -DCMAKE_TOOLCHAIN_FILE=cmake/esp32s3_qemu.cmake && cmake --build build_esp32s3
```

There is no separate lint step. The test binary compiles the kernel sources directly (`#include "../src/*.c"`) with port stubs, so a successful build implies the kernel compiles cleanly.

## Architecture Overview

This is a small preemptive RTOS written in C11 with no dynamic allocation.

**Layer split:**
- `src/` — architecture-independent kernel (task, sem, mutex, queue, timer, list)
- `include/` — public API headers (users include only `rtos.h`)
- `src/port.h` — internal port contract (6 functions + 2 callbacks every port must implement)
- `port/<arch>/port.c` + `port_asm.S` — one directory per target; CMake selects based on toolchain

**Scheduler internals (`src/task.c`):**
- Per-priority ready queues (`g_ready[RTOS_MAX_PRIORITIES]`) plus a `g_ready_bitmap`; `__builtin_ctz` on the bitmap finds the highest-priority ready task in O(1).
- A single delay-sorted blocked list (`g_blocked`).
- `g_current` is a non-`static` global so port assembly can access it directly.
- `rtos_context_switch()` and `rtos_current_tcb_ptr()` / `rtos_next_task()` are the only kernel symbols exposed to port code at link time.

**Tick path:** port ISR → `rtos_tick_handler()` → unblocks tasks + fires timers → `port_request_reschedule()` → deferred context switch (PendSV on ARM, inline on AVR/RISC-V).

**Static allocation pattern:** every kernel object (`rtos_tcb_t`, `rtos_sem_t`, `rtos_mutex_t`, `rtos_queue_t`, `rtos_timer_t`) is allocated by the *caller* as a static variable and passed by pointer to the `rtos_*_create` function. The kernel never calls `malloc`.

## Key Conventions

**Naming:**
- Public API uses `rtos_` prefix with full snake_case words: `rtos_task_create`, `rtos_task_delay`, `rtos_semaphore_take`, `rtos_mutex_lock`, `rtos_queue_send`, `rtos_timer_start`, `rtos_start`.
- Internal kernel symbols use lowercase with underscores: `ready_add`, `scheduler_pick_next`, `rtos_tick_handler`.
- Port-layer symbols are prefixed `port_`: `port_init`, `port_init_stack`, `port_enter_critical`, `port_exit_critical`, `port_request_reschedule`, `port_start_first_task`.

**Priority:** lower numeric value = higher priority (0 is highest). `RTOS_MAX_PRIORITIES - 1` is reserved for the idle task.

**Handles:** `rtos_handle_t` is `void *`; it always points to the underlying struct (e.g., the `rtos_tcb_t`). Passing `NULL` to task functions (`rtos_task_delete`, `rtos_task_suspend`) targets the current task.

**Return codes:** functions that can fail return `RTOS_OK` (0), `RTOS_ERR` (-1), or `RTOS_TIMEOUT` (-2); `rtos_*_create` functions return `NULL` on failure.

**`stack_words` parameter:** counts units of `RTOS_STACK_BYTES_PER_WORD` (4 on 32-bit, 1 on AVR). Declare stacks as `static rtos_stack_t stack[N]` — the typedef resolves to `uint32_t` on 32-bit targets and `uint8_t` on AVR.

**Intrusive lists:** `rtos_tcb_t` has a `next` pointer; the list implementation in `src/list.c` is intrusive and used only inside the scheduler. Queue and timer internal nodes are embedded in their own structs too.

**Adding a new port:**
1. Create `port/<arch>/port.c` implementing all 6 `port_` functions.
2. Create `port/<arch>/port_asm.S` for the context-switch handler (saves/restores the full register set and calls `rtos_context_switch()`).
3. Add a toolchain file in `cmake/` and a new `elseif` branch in `CMakeLists.txt`.
4. The toolchain file must set `CMAKE_SYSTEM_PROCESSOR` to a unique string that CMake detects.

**Testing new kernel logic:** add test functions to `test/test_rtos.c` using the `TEST(expr)` macro; register them in `main()`. Port stubs at the top of that file stub out all `port_` functions.

## Workflow

Do not run `git commit` or `git push`. Leave all commits to the developer so they can review changes before checking in.

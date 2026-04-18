# rtos

A small, portable, hobby-grade RTOS written in C.

**Design goals:**
- Preemptive, priority-based scheduling (round-robin within equal priorities)
- Static memory allocation only — no `malloc`, no surprises
- FreeRTOS-inspired API (`xTaskCreate`, `vTaskDelay`, `xSemaphoreTake`, …)
- Easily portable — architecture-specific code isolated in `port/<arch>/`
- Host-testable kernel logic (unit tests run on the development machine)

**Primary target:** RP2040 (ARM Cortex-M0+)  
**Planned:** RISC-V (stubs in `port/riscv/`)

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
│   ├── arm_cm0plus/  # SysTick, PendSV context switch
│   └── riscv/        # Placeholder (to be implemented)
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
| `RTOS_TICK_RATE_HZ` | 1000 | SysTick frequency |
| `RTOS_MAX_TIMERS` | 8 | Maximum software timers |
| `RTOS_TASK_NAME_LEN` | 16 | Task/timer name buffer size |

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

xTaskCreate(&my_tcb, my_stack, 64, my_task_func, NULL, "myTask", 1);
```

---

## API quick-reference

### Kernel
```c
void     vRTOSStart(void);           // start scheduler — never returns
uint32_t xTaskGetTickCount(void);
```

### Tasks
```c
// All storage (TCB + stack) must be static/global — provided by the caller.
static rtos_tcb_t my_tcb;
static uint32_t   my_stack[256];

rtos_handle_t h = xTaskCreate(&my_tcb, my_stack, 256,
                               my_task_func, NULL, "myTask", 3);
void vTaskDelay(uint32_t ticks);     // block for N ticks
void vTaskYield(void);
void vTaskSuspend(rtos_handle_t);
void vTaskResume(rtos_handle_t);
void vTaskDelete(rtos_handle_t);     // pass NULL for current task
```

### Semaphores
```c
static rtos_sem_t my_sem;
rtos_handle_t s = xSemaphoreCreateBinary(&my_sem);
// or:
rtos_handle_t s = xSemaphoreCreateCounting(&my_sem, max, initial);

xSemaphoreGive(s);
xSemaphoreTake(s, RTOS_WAIT_FOREVER);   // returns RTOS_OK or RTOS_TIMEOUT
xSemaphoreGiveFromISR(s);
```

### Mutexes
```c
static rtos_mutex_t my_mutex;
rtos_handle_t m = xMutexCreate(&my_mutex);

xMutexLock(m, RTOS_WAIT_FOREVER);
xMutexUnlock(m);
```

### Message queues
```c
static rtos_queue_t my_queue;
static uint8_t      my_buf[8 * sizeof(int)];   // 8 ints
rtos_handle_t q = xQueueCreate(&my_queue, my_buf, sizeof(int), 8);

int val = 42;
xQueueSend(q, &val, RTOS_WAIT_FOREVER);
xQueueReceive(q, &val, RTOS_WAIT_FOREVER);
xQueueSendFromISR(q, &val);
xQueueMessagesWaiting(q);
```

### Software timers
```c
static rtos_timer_t my_timer;
rtos_handle_t t = xTimerCreate(&my_timer, "blink", 500, /*periodic=*/1, blink_cb);
xTimerStart(t);
xTimerStop(t);
xTimerReset(t);
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
        xSemaphoreTake(sem, RTOS_WAIT_FOREVER);
        // do work
    }
}

static void task2(void *arg)
{
    rtos_handle_t sem = (rtos_handle_t)arg;
    for (;;) {
        vTaskDelay(1000);   // 1 second at 1000 Hz
        xSemaphoreGive(sem);
    }
}

int main(void)
{
    rtos_handle_t sem = xSemaphoreCreateBinary(&ready_sem);

    xTaskCreate(&task1_tcb, task1_stack, 256, task1, sem, "task1", 1);
    xTaskCreate(&task2_tcb, task2_stack, 256, task2, sem, "task2", 2);

    vRTOSStart();   // never returns
}
```

---

## Porting to a new architecture

1. Copy `port/arm_cm0plus/` to `port/<your-arch>/`
2. Implement `port.c`: `port_init`, `port_enter_critical`, `port_exit_critical`, `port_request_reschedule`, `port_start_first_task`, `port_init_stack`
3. Implement `port_asm.S`: the context-switch handler
4. Update `CMakeLists.txt` to select the right port sources

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

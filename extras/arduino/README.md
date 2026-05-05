# QuartzRTOS Arduino Library

A small, fast, preemptive RTOS for Arduino — no `malloc`, no surprises.

**Supported boards:** Arduino Uno, Nano, Mega (ATmega328P / ATmega2560)  
**Supported IDE:** Arduino IDE 1.8+ and Arduino IDE 2.x

---

## ⚠️ Timer1 Conflict — Read This First

QuartzRTOS uses **Timer1 in CTC mode** to generate its tick interrupt on AVR.
The following Arduino features and libraries **cannot be used** in the same sketch:

| Incompatible feature | Reason |
|----------------------|--------|
| `tone()` / `noTone()` | Uses Timer2 on Uno/Nano (OK), but uses Timer1 on some boards |
| `Servo` library | Uses Timer1 on AVR |
| `TimerOne` library | Directly controls Timer1 |
| `IRremote` (some configs) | May use Timer1 |

If you need PWM tone or servo control alongside an RTOS, consider using
FreeRTOS for Arduino instead, which has wider library compatibility.

---

## Installation

**Option A — ZIP install (recommended for local use)**

1. Download or clone the Quartz repository.
2. In Arduino IDE: **Sketch → Include Library → Add .ZIP Library**
3. Navigate to the `extras/arduino/` directory inside the repo and click **Open**.

**Option B — Manual install**

Copy the `extras/arduino/` directory into your Arduino `libraries/` folder and
rename it to `QuartzRTOS`.

---

## Quick Start

```cpp
#include <rtos.h>

static rtos_tcb_t   my_tcb;
static rtos_stack_t my_stack[96];

static void my_task(void *arg) {
    pinMode(LED_BUILTIN, OUTPUT);
    for (;;) {
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        rtos_task_delay(500);   // 500 ticks = 500 ms
    }
}

void setup() {
    rtos_task_create(&my_tcb, my_stack, 96, my_task, NULL, "blink", 1);
    rtos_start();   // never returns
}

void loop() { /* unreachable */ }
```

> **Important:** Call `rtos_start()` at the end of `setup()`. It never returns,
> so `loop()` will never execute. Put all your application logic in tasks.

---

## Key Concepts

### Static allocation

Every kernel object is allocated by **you** as a static variable. The RTOS
never calls `malloc`. This makes memory usage predictable and eliminates
heap fragmentation.

```cpp
static rtos_tcb_t   tcb;          // Task Control Block
static rtos_stack_t stack[128];   // Task stack (128 bytes on AVR)
static rtos_sem_t   sem;          // Semaphore
static rtos_mutex_t mtx;          // Mutex
```

### Stack sizing

On AVR each stack element is **1 byte** (`rtos_stack_t` = `uint8_t`).
The stack size in `rtos_task_create()` is in bytes.

A minimal task needs about 60–80 bytes. Tasks that call functions or use local
variables need more. Start with 128 bytes and reduce if SRAM is tight.

### Tick rate

The default tick rate is **1000 Hz** (1 ms per tick). Delays and timeouts are
in ticks. To halve the tick rate and save CPU overhead:

```cpp
// Before #include <rtos.h>:
#define RTOS_TICK_RATE_HZ 500
```

### Priority

Lower number = higher priority. `0` is the highest priority a user task can
use. The idle task always runs at the lowest priority. Typical values: 0–3.

---

## Configuration

Override any default by defining the symbol **before** `#include <rtos.h>`:

| Symbol | Default (AVR) | Description |
|--------|---------------|-------------|
| `RTOS_MAX_TASKS` | 8 | Maximum number of tasks (including idle) |
| `RTOS_MAX_PRIORITIES` | 8 | Number of priority levels |
| `RTOS_TICK_RATE_HZ` | 1000 | Tick frequency in Hz |
| `RTOS_MAX_TIMERS` | 8 | Maximum software timers |
| `RTOS_TASK_NAME_LEN` | 16 | Max length of a task/timer name |
| `RTOS_ENABLE_TASK_DELETE` | 1 | Enable `rtos_task_delete()` |
| `RTOS_ENABLE_TASK_SUSPEND` | 1 | Enable suspend/resume |
| `RTOS_ENABLE_SOFTWARE_TIMERS` | 1 | Enable software timers |
| `RTOS_ENABLE_PRIORITY_INHERITANCE` | 1 | Mutex priority inheritance |

Example — minimal sketch for Uno with very limited SRAM:

```cpp
#define RTOS_MAX_TASKS             4
#define RTOS_MAX_TIMERS            0
#define RTOS_ENABLE_TASK_DELETE    0
#define RTOS_ENABLE_TASK_SUSPEND   0
#define RTOS_ENABLE_SOFTWARE_TIMERS 0
#include <rtos.h>
```

---

## API Reference (summary)

### Tasks
```c
rtos_handle_t rtos_task_create(rtos_tcb_t *tcb, rtos_stack_t *stack,
                                size_t stack_words, void (*func)(void *),
                                void *arg, const char *name, uint8_t priority);
void rtos_task_delay(rtos_tick_t ticks);
void rtos_task_delay_until(rtos_tick_t *last_wake, rtos_tick_t period);
void rtos_task_yield(void);
rtos_tick_t rtos_task_tick_count(void);
void rtos_start(void);
```

### Semaphores
```c
rtos_handle_t rtos_semaphore_create_binary(rtos_sem_t *sem);
rtos_handle_t rtos_semaphore_create_counting(rtos_sem_t *sem,
                                              uint32_t max, uint32_t initial);
int  rtos_semaphore_take(rtos_handle_t sem, rtos_tick_t timeout);
void rtos_semaphore_give(rtos_handle_t sem);
void rtos_semaphore_give_from_isr(rtos_handle_t sem);
```

### Mutexes
```c
rtos_handle_t rtos_mutex_create(rtos_mutex_t *mutex);
int  rtos_mutex_lock(rtos_handle_t mutex, rtos_tick_t timeout);
int  rtos_mutex_unlock(rtos_handle_t mutex);
```

### Queues
```c
rtos_handle_t rtos_queue_create(rtos_queue_t *q, void *buf,
                                 size_t item_size, size_t capacity);
int rtos_queue_send(rtos_handle_t q, const void *item, rtos_tick_t timeout);
int rtos_queue_receive(rtos_handle_t q, void *item, rtos_tick_t timeout);
```

---

## ESP32-S3 Support

The standard **Arduino ESP32 core** runs on top of ESP-IDF, which already
provides FreeRTOS. QuartzRTOS cannot replace FreeRTOS in that environment.

ESP32-S3 support via Arduino is currently **not available**. The Quartz
ESP32-S3 port targets bare-metal QEMU builds only. Tracking issue:
https://github.com/mseminatore/quartz

---

## Source & License

Source: https://github.com/mseminatore/quartz  
License: See the LICENSE file.

The `extras/arduino/src/` directory is assembled by `tools/package_arduino.py`
from the main source tree. To regenerate after modifying the kernel:

```sh
python3 tools/package_arduino.py
```

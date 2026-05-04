# Quartz — repository layout

```
quartz/
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
│   ├── arm_cm0plus/       # SysTick, PendSV context switch (M0+ — used by RP2040 and bare CM0+)
│   ├── arm_cm4/           # SysTick, BASEPRI critical sections, PendSV (shared by M3/M4/M7)
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
│   ├── avr_atmega328p.cmake       # avr-gcc toolchain file
│   ├── riscv32_clint.cmake        # riscv64-unknown-elf-gcc toolchain file (rv32imac_zicsr)
│   ├── esp32s3_qemu.cmake         # xtensa-esp32s3-elf-gcc toolchain file (Call0 ABI)
│   ├── arm_cm0plus_generic.cmake  # arm-none-eabi-gcc toolchain file (bare Cortex-M0+)
│   ├── arm_cm3.cmake              # arm-none-eabi-gcc toolchain file (Cortex-M3)
│   ├── arm_cm4.cmake              # arm-none-eabi-gcc toolchain file (Cortex-M4F)
│   └── arm_cm7.cmake              # arm-none-eabi-gcc toolchain file (Cortex-M7, fpv5-sp-d16)
├── extern/
│   └── testy/        # Testy unit-test framework (git submodule)
├── test/
│   └── test_rtos.c   # Host-side unit tests
└── CMakeLists.txt
```

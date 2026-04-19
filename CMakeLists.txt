cmake_minimum_required(VERSION 3.13)

# ---------------------------------------------------------------------------
# Board/SDK bootstrap — must happen before project()
# ---------------------------------------------------------------------------
# For RP2040 with the Pico SDK:
#   cmake -B build -DPICO_SDK_PATH=~/pico-sdk
#
# For AVR ATmega328P:
#   cmake -B build_avr -DCMAKE_TOOLCHAIN_FILE=cmake/avr_atmega328p.cmake
#
# For RISC-V (RV32IMAC, CLINT timer — QEMU virt / SiFive FE310):
#   cmake -B build_rv32 -DCMAKE_TOOLCHAIN_FILE=cmake/riscv32_clint.cmake
#
# For ESP32-S3 (Xtensa LX7, Call0 ABI — Espressif QEMU):
#   cmake -B build_esp32s3 -DCMAKE_TOOLCHAIN_FILE=cmake/esp32s3_qemu.cmake
#
# For host unit tests (default, no hardware):
#   cmake -B build && cmake --build build && ./build/rtos_test

if(DEFINED ENV{PICO_SDK_PATH} OR DEFINED PICO_SDK_PATH)
    if(NOT DEFINED PICO_SDK_PATH)
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    endif()
    include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
    set(RTOS_TARGET_PICO ON)
else()
    set(RTOS_TARGET_PICO OFF)
endif()

# Detect AVR cross-compile (toolchain file sets CMAKE_SYSTEM_PROCESSOR=avr)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "avr")
    set(RTOS_TARGET_AVR ON)
else()
    set(RTOS_TARGET_AVR OFF)
endif()

# Detect RISC-V cross-compile (toolchain file sets CMAKE_SYSTEM_PROCESSOR=riscv)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "riscv")
    set(RTOS_TARGET_RISCV ON)
else()
    set(RTOS_TARGET_RISCV OFF)
endif()

# Detect ESP32-S3 cross-compile (toolchain file sets CMAKE_SYSTEM_PROCESSOR=xtensa-esp32s3)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "xtensa-esp32s3")
    set(RTOS_TARGET_ESP32S3 ON)
else()
    set(RTOS_TARGET_ESP32S3 OFF)
endif()

project(rtos C ASM)
set(CMAKE_C_STANDARD 11)

if(RTOS_TARGET_PICO)
    pico_sdk_init()
endif()

# ---------------------------------------------------------------------------
# RTOS library (architecture-independent kernel sources)
# ---------------------------------------------------------------------------

set(RTOS_KERNEL_SOURCES
    src/list.c
    src/task.c
    src/sem.c
    src/mutex.c
    src/queue.c
    src/timer.c
)

if(RTOS_TARGET_PICO)
    set(RTOS_PORT_SOURCES
        port/arm_cm0plus/port.c
        port/arm_cm0plus/port_asm.S
    )
elseif(RTOS_TARGET_AVR)
    set(RTOS_PORT_SOURCES
        port/avr_atmega/port.c
    )
elseif(RTOS_TARGET_RISCV)
    set(RTOS_PORT_SOURCES
        port/riscv/port.c
        port/riscv/port_asm.S
    )
elseif(RTOS_TARGET_ESP32S3)
    set(RTOS_PORT_SOURCES
        port/xtensa_esp32s3/port.c
        port/xtensa_esp32s3/port_asm.S
    )
else()
    set(RTOS_PORT_SOURCES "")
endif()

add_library(rtos STATIC
    ${RTOS_KERNEL_SOURCES}
    ${RTOS_PORT_SOURCES}
)

target_include_directories(rtos PUBLIC
    include
    src          # for internal port.h and list.h
)

target_compile_options(rtos PRIVATE -fno-builtin)

if(RTOS_TARGET_PICO)
    target_link_libraries(rtos pico_stdlib)
    target_compile_definitions(rtos PRIVATE RTOS_CPU_HZ=125000000)
endif()

# For AVR, recommend reducing config values in your application's rtos_config.h:
#   RTOS_MAX_TASKS          4   (2 KB SRAM is tight)
#   RTOS_MAX_TIMERS         4
#   RTOS_IDLE_STACK_WORDS  16   (16 bytes)
#   RTOS_STACK_BYTES_PER_WORD 1 (one byte per "word" on AVR)

# ---------------------------------------------------------------------------
# Host-side unit tests (runs on the development machine, no hardware needed)
# ---------------------------------------------------------------------------

if(NOT RTOS_TARGET_PICO AND NOT RTOS_TARGET_AVR AND NOT RTOS_TARGET_RISCV AND NOT RTOS_TARGET_ESP32S3)
    enable_testing()

    add_executable(rtos_test test/test_rtos.c)
    target_include_directories(rtos_test PRIVATE include src)
    target_compile_options(rtos_test PRIVATE -fno-builtin)

    add_test(NAME rtos_test COMMAND rtos_test)
endif()

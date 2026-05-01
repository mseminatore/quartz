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
#
# For ARM Cortex-M4F (generic bare-metal, e.g. STM32F4xx):
#   cmake -B build_cm4 -DCMAKE_TOOLCHAIN_FILE=cmake/arm_cm4.cmake
#
# As a subdirectory of another CMake project (e.g. a Pico SDK app):
#   add_subdirectory(deps/rtos)   # Pico SDK must already be initialised

# ---------------------------------------------------------------------------
# Standalone vs subdirectory — bootstrap only when we are the root project
# ---------------------------------------------------------------------------

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # Running as the root project: handle SDK / toolchain bootstrapping.

    if(DEFINED ENV{PICO_SDK_PATH} OR DEFINED PICO_SDK_PATH)
        if(NOT DEFINED PICO_SDK_PATH)
            set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
        endif()
        include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
        set(RTOS_TARGET_PICO ON)
    else()
        set(RTOS_TARGET_PICO OFF)
    endif()

    project(rtos C ASM)
    set(CMAKE_C_STANDARD 11)

    if(RTOS_TARGET_PICO)
        pico_sdk_init()
    endif()

else()
    # Running as add_subdirectory(): parent owns project/SDK init.
    # Detect Pico SDK by checking whether pico_sdk_init() has been called.
    if(DEFINED PICO_SDK_VERSION_STRING OR TARGET pico_stdlib)
        set(RTOS_TARGET_PICO ON)
    else()
        set(RTOS_TARGET_PICO OFF)
    endif()

endif()

# Architecture detection (works in both standalone and subdirectory builds
# because the toolchain file sets CMAKE_SYSTEM_PROCESSOR before project()).
#
# Selection precedence (highest first):
#   1. -DRTOS_PORT=<name>          (explicit cache variable; recommended)
#   2. CMAKE_SYSTEM_PROCESSOR      (set by the toolchain file; legacy path)
#
# Valid RTOS_PORT values: pico, avr, riscv, esp32s3, cm4, host (no port).
# When unset, CMAKE_SYSTEM_PROCESSOR is consulted for backward compatibility.

set(RTOS_PORT "" CACHE STRING
    "Port to build (pico|avr|riscv|esp32s3|cm4|host). Empty = auto-detect.")
set_property(CACHE RTOS_PORT PROPERTY STRINGS "" pico avr riscv esp32s3 cm4 host)

# Resolve effective port: explicit RTOS_PORT wins; otherwise infer from
# CMAKE_SYSTEM_PROCESSOR (set by the toolchain files).
if(RTOS_PORT)
    set(_rtos_port "${RTOS_PORT}")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "avr")
    set(_rtos_port "avr")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "riscv")
    set(_rtos_port "riscv")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "xtensa-esp32s3")
    set(_rtos_port "esp32s3")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm_cm4")
    set(_rtos_port "cm4")
elseif(RTOS_TARGET_PICO)
    set(_rtos_port "pico")
else()
    set(_rtos_port "host")
endif()

set(RTOS_TARGET_AVR     OFF)
set(RTOS_TARGET_RISCV   OFF)
set(RTOS_TARGET_ESP32S3 OFF)
set(RTOS_TARGET_CM4     OFF)

if(_rtos_port STREQUAL "pico")
    set(RTOS_TARGET_PICO ON)
elseif(_rtos_port STREQUAL "avr")
    set(RTOS_TARGET_AVR ON)
elseif(_rtos_port STREQUAL "riscv")
    set(RTOS_TARGET_RISCV ON)
elseif(_rtos_port STREQUAL "esp32s3")
    set(RTOS_TARGET_ESP32S3 ON)
elseif(_rtos_port STREQUAL "cm4")
    set(RTOS_TARGET_CM4 ON)
elseif(_rtos_port STREQUAL "host")
    # No port sources; host build (unit tests).
else()
    message(FATAL_ERROR
        "Unknown RTOS_PORT='${_rtos_port}'. "
        "Valid values: pico avr riscv esp32s3 cm4 host.")
endif()

message(STATUS "rtos: selected port = ${_rtos_port}")

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
elseif(RTOS_TARGET_CM4)
    set(RTOS_PORT_SOURCES
        port/arm_cm4/port.c
        port/arm_cm4/port_asm.S
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

if(MSVC)
    target_compile_options(rtos PRIVATE /W3)
else()
    target_compile_options(rtos PRIVATE -fno-builtin)
endif()

if(RTOS_TARGET_PICO)
    target_link_libraries(rtos pico_stdlib)
    # Allow the parent project to override RTOS_CPU_HZ (e.g. Pico W uses 133 MHz).
    # The default is 125 MHz (standard RP2040 / Pico).
    if(NOT DEFINED RTOS_CPU_HZ)
        set(RTOS_CPU_HZ 125000000)
    endif()
    target_compile_definitions(rtos PRIVATE RTOS_CPU_HZ=${RTOS_CPU_HZ})
elseif(RTOS_TARGET_CM4)
    # Allow the parent project to override RTOS_CPU_HZ for specific M4 boards.
    # Default: 168 MHz (STM32F4xx).  Override e.g. -DRTOS_CPU_HZ=72000000 for STM32F3xx.
    if(DEFINED RTOS_CPU_HZ)
        target_compile_definitions(rtos PRIVATE RTOS_CPU_HZ=${RTOS_CPU_HZ})
    endif()
    # Allow overriding the BASEPRI critical-section threshold.
    if(DEFINED RTOS_BASEPRI_VALUE)
        target_compile_definitions(rtos PRIVATE RTOS_BASEPRI_VALUE=${RTOS_BASEPRI_VALUE})
    endif()
    # Propagate the FPU knob to both C and ASM (port_asm.S guards on it).
    # CMake routes target_compile_definitions to all language drivers, so this
    # reaches the assembler without us having to #include rtos_config.h from .S.
    if(RTOS_CM4_FPU)
        target_compile_definitions(rtos PUBLIC RTOS_CM4_FPU=1)
    endif()
endif()

# Pass optional debug/instrumentation overrides from the parent project.
foreach(_def
        RTOS_STACK_OVERFLOW_CHECK
        RTOS_STACK_WATERMARK
        RTOS_ENABLE_TRACE
        RTOS_ENABLE_RUNTIME_STATS
        RTOS_TICKLESS_IDLE
        RTOS_NUM_CORES)
    if(DEFINED ${_def})
        target_compile_definitions(rtos PUBLIC ${_def}=${${_def}})
    endif()
endforeach()

# For AVR, recommend reducing config values in your application's rtos_config.h:
#   RTOS_MAX_TASKS          4   (2 KB SRAM is tight)
#   RTOS_MAX_TIMERS         4
#   RTOS_IDLE_STACK_WORDS  16   (16 bytes)
#   RTOS_STACK_BYTES_PER_WORD 1 (one byte per "word" on AVR)

# ---------------------------------------------------------------------------
# Host-side unit tests (runs on the development machine, no hardware needed)
# ---------------------------------------------------------------------------

if(NOT RTOS_TARGET_PICO AND NOT RTOS_TARGET_AVR AND NOT RTOS_TARGET_RISCV AND NOT RTOS_TARGET_ESP32S3 AND NOT RTOS_TARGET_CM4)
    enable_testing()

    add_executable(rtos_test test/test_rtos.c test/test_main.c)
    target_include_directories(rtos_test PRIVATE include src test)
    if(MSVC)
        target_compile_options(rtos_test PRIVATE /W3)
    else()
        target_compile_options(rtos_test PRIVATE -fno-builtin)
    endif()

    add_test(NAME rtos_test COMMAND rtos_test)

    # ---------------------------------------------------------------------------
    # Sample programs (host simulation — hardware calls are stubbed inside each file)
    # ---------------------------------------------------------------------------

    foreach(sample blink producer_consumer mutex_shared_resource uart_echo adc_pipeline)
        add_executable(sample_${sample} samples/${sample}.c
                       ${RTOS_KERNEL_SOURCES}
                       port/host/port.c)
        target_include_directories(sample_${sample} PRIVATE include src)
        if(MSVC)
            target_compile_options(sample_${sample} PRIVATE /W3)
        else()
            target_compile_options(sample_${sample} PRIVATE -fno-builtin)
        endif()
        if(sample STREQUAL "adc_pipeline")
            target_link_libraries(sample_${sample} m)  # sinf for host stub
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Pico SDK samples (RP2040 / Pico W)
# ---------------------------------------------------------------------------

if(RTOS_TARGET_PICO)
    # uart_echo and adc_pipeline — work on any RP2040 or Pico W
    foreach(sample uart_echo adc_pipeline)
        add_executable(sample_${sample} samples/${sample}.c)
        target_link_libraries(sample_${sample} rtos pico_stdlib hardware_adc)
        target_include_directories(sample_${sample} PRIVATE include)
        if(sample STREQUAL "adc_pipeline")
            target_link_libraries(sample_${sample} m)
        endif()
        pico_add_extra_outputs(sample_${sample})
        pico_enable_stdio_usb(sample_${sample} 1)
        pico_enable_stdio_uart(sample_${sample} 0)
    endforeach()

    # wifi_http — Pico W only (requires CYW43 WiFi chip)
    if(PICO_BOARD STREQUAL "pico_w")
        add_executable(sample_wifi_http samples/wifi_http.c)
        target_link_libraries(sample_wifi_http
            rtos
            pico_stdlib
            pico_cyw43_arch_lwip_threadsafe_background
        )
        target_include_directories(sample_wifi_http PRIVATE include samples)
        target_compile_definitions(sample_wifi_http PRIVATE
            PICO_W=1
            WIFI_SSID="${WIFI_SSID}"
            WIFI_PASSWORD="${WIFI_PASSWORD}"
        )
        pico_add_extra_outputs(sample_wifi_http)
        pico_enable_stdio_usb(sample_wifi_http 1)
        pico_enable_stdio_uart(sample_wifi_http 0)
    endif()
endif()

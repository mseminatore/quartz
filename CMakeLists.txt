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
#   cmake -B build && cmake --build build && ./build/quartz_test
#
# For ARM Cortex-M4F (generic bare-metal, e.g. STM32F4xx):
#   cmake -B build_cm4 -DCMAKE_TOOLCHAIN_FILE=cmake/arm_cm4.cmake
#
# As a subdirectory of another CMake project (e.g. a Pico SDK app):
#   add_subdirectory(deps/quartz)   # Pico SDK must already be initialised

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

    include(cmake/version.cmake)
    project(quartz VERSION "${RTOS_VERSION_MAJOR}.${RTOS_VERSION_MINOR}.${RTOS_VERSION_PATCH}" LANGUAGES C ASM)
    set(CMAKE_C_STANDARD 11)

    # Stamp include/rtos_version.h from the .in template
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/include/rtos_version.h.in"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/rtos_version.h"
        @ONLY
    )

    if(RTOS_TARGET_PICO)
        pico_sdk_init()
    endif()

    # Interactive/scriptable editor for include/rtos_config.h.
    #   cmake --build build --target menuconfig
    add_custom_target(menuconfig
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/menuconfig.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        USES_TERMINAL
    )

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
    "Port to build (pico|cm0plus|cm3|cm4|cm7|avr|riscv|esp32s3|host). Empty = auto-detect.")
set_property(CACHE RTOS_PORT PROPERTY STRINGS "" pico cm0plus cm3 cm4 cm7 avr riscv esp32s3 host)

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
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm_cm0plus")
    set(_rtos_port "cm0plus")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm_cm3")
    set(_rtos_port "cm3")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm_cm4")
    set(_rtos_port "cm4")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm_cm7")
    set(_rtos_port "cm7")
elseif(RTOS_TARGET_PICO)
    set(_rtos_port "pico")
else()
    set(_rtos_port "host")
endif()

set(RTOS_TARGET_AVR     OFF)
set(RTOS_TARGET_RISCV   OFF)
set(RTOS_TARGET_ESP32S3 OFF)
set(RTOS_TARGET_CM0PLUS OFF)
set(RTOS_TARGET_CM4     OFF)

if(_rtos_port STREQUAL "pico")
    set(RTOS_TARGET_PICO    ON)
    set(RTOS_TARGET_CM0PLUS ON)
elseif(_rtos_port STREQUAL "cm0plus")
    set(RTOS_TARGET_CM0PLUS ON)
elseif(_rtos_port STREQUAL "avr")
    set(RTOS_TARGET_AVR ON)
elseif(_rtos_port STREQUAL "riscv")
    set(RTOS_TARGET_RISCV ON)
elseif(_rtos_port STREQUAL "esp32s3")
    set(RTOS_TARGET_ESP32S3 ON)
elseif(_rtos_port STREQUAL "cm3" OR _rtos_port STREQUAL "cm4" OR _rtos_port STREQUAL "cm7")
    # All three Cortex-M variants share the same port sources (Thumb-2 ISA,
    # BASEPRI critical sections, identical PendSV context-switch shape).
    # The toolchain file selects the right -mcpu/-mfpu flags.
    set(RTOS_TARGET_CM4 ON)
elseif(_rtos_port STREQUAL "host")
    # No port sources; host build (unit tests).
else()
    message(FATAL_ERROR
        "Unknown RTOS_PORT='${_rtos_port}'. "
        "Valid values: pico cm0plus cm3 cm4 cm7 avr riscv esp32s3 host.")
endif()

message(STATUS "rtos: selected port = ${_rtos_port}")

# ---------------------------------------------------------------------------
# Quartz library (architecture-independent kernel sources)
# ---------------------------------------------------------------------------

set(RTOS_KERNEL_SOURCES
    src/list.c
    src/task.c
    src/sem.c
    src/mutex.c
    src/queue.c
    src/timer.c
    src/eventgroup.c
)

# ---------------------------------------------------------------------------
# Optional tracing backend selection
#   -DRTOS_TRACE_BACKEND=none      (default, no overhead)
#   -DRTOS_TRACE_BACKEND=chrome    (ring buffer → Chrome Trace Event JSON)
#   -DRTOS_TRACE_BACKEND=sysview   (reserved for SEGGER SystemView; not yet)
# ---------------------------------------------------------------------------
set(RTOS_TRACE_BACKEND "none" CACHE STRING
    "Tracing backend: none | chrome | sysview")
set_property(CACHE RTOS_TRACE_BACKEND PROPERTY STRINGS none chrome sysview)

if(RTOS_TRACE_BACKEND STREQUAL "chrome")
    list(APPEND RTOS_KERNEL_SOURCES src/trace_chrome.c)
endif()

if(RTOS_TARGET_CM0PLUS)
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

add_library(quartz STATIC
    ${RTOS_KERNEL_SOURCES}
    ${RTOS_PORT_SOURCES}
)

target_include_directories(quartz PUBLIC
    include
    src          # for internal port.h and list.h
)

if(MSVC)
    target_compile_options(quartz PRIVATE /W3)
else()
    target_compile_options(quartz PRIVATE -fno-builtin)
endif()

if(RTOS_TARGET_PICO)
    target_link_libraries(quartz pico_stdlib)
    # Allow the parent project to override RTOS_CPU_HZ (e.g. Pico W uses 133 MHz).
    # The default is 125 MHz (standard RP2040 / Pico).
    if(NOT DEFINED RTOS_CPU_HZ)
        set(RTOS_CPU_HZ 125000000)
    endif()
    target_compile_definitions(quartz PRIVATE RTOS_CPU_HZ=${RTOS_CPU_HZ})
elseif(RTOS_TARGET_CM4)
    # Allow the parent project to override RTOS_CPU_HZ for specific M4 boards.
    # Default: 168 MHz (STM32F4xx).  Override e.g. -DRTOS_CPU_HZ=72000000 for STM32F3xx.
    if(DEFINED RTOS_CPU_HZ)
        target_compile_definitions(quartz PRIVATE RTOS_CPU_HZ=${RTOS_CPU_HZ})
    endif()
    # Allow overriding the BASEPRI critical-section threshold.
    if(DEFINED RTOS_BASEPRI_VALUE)
        target_compile_definitions(quartz PRIVATE RTOS_BASEPRI_VALUE=${RTOS_BASEPRI_VALUE})
    endif()
    # Propagate the FPU knob to both C and ASM (port_asm.S guards on it).
    # CMake routes target_compile_definitions to all language drivers, so this
    # reaches the assembler without us having to #include rtos_config.h from .S.
    if(RTOS_CM4_FPU)
        target_compile_definitions(quartz PUBLIC RTOS_CM4_FPU=1)
    endif()
endif()

# Pass optional debug/instrumentation overrides from the parent project.
foreach(_def
        RTOS_STACK_OVERFLOW_CHECK
        RTOS_STACK_WATERMARK
        RTOS_ENABLE_TRACE
        RTOS_ENABLE_RUNTIME_STATS
        RTOS_TICKLESS_IDLE
        RTOS_NUM_CORES
        RTOS_TRACE_BUFFER_BYTES)
    if(DEFINED ${_def})
        target_compile_definitions(quartz PUBLIC ${_def}=${${_def}})
    endif()
endforeach()

# Activate the chosen trace backend (also turns on RTOS_ENABLE_TRACE so that
# the rtos_trace_* hook macros in rtos_trace.h expand to real calls).
if(RTOS_TRACE_BACKEND STREQUAL "chrome")
    target_compile_definitions(quartz PUBLIC
        RTOS_ENABLE_TRACE=1
        RTOS_TRACE_BACKEND_CHROME=1)
endif()

# For AVR, recommend reducing config values in your application's rtos_config.h:
#   RTOS_MAX_TASKS          4   (2 KB SRAM is tight)
#   RTOS_MAX_TIMERS         4
#   RTOS_IDLE_STACK_WORDS  16   (16 bytes)
#   RTOS_STACK_BYTES_PER_WORD 1 (one byte per "word" on AVR)

# ---------------------------------------------------------------------------
# Host-side unit tests (runs on the development machine, no hardware needed)
# ---------------------------------------------------------------------------

if(NOT RTOS_TARGET_PICO AND NOT RTOS_TARGET_CM0PLUS AND NOT RTOS_TARGET_AVR AND NOT RTOS_TARGET_RISCV AND NOT RTOS_TARGET_ESP32S3 AND NOT RTOS_TARGET_CM4)
    enable_testing()

    add_executable(quartz_test test/test_rtos.c extern/testy/test_main.c)
    target_include_directories(quartz_test PRIVATE include src extern/testy)
    if(MSVC)
        target_compile_options(quartz_test PRIVATE /W3)
    else()
        target_compile_options(quartz_test PRIVATE -fno-builtin)
    endif()

    add_test(NAME quartz_test COMMAND quartz_test)

    # ---------------------------------------------------------------------------
    # Sample programs (host simulation — hardware calls are stubbed inside each file)
    # ---------------------------------------------------------------------------

    foreach(sample blink producer_consumer mutex_shared_resource uart_echo adc_pipeline timer_demo runtime_stats event_group_demo)
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
        if(sample STREQUAL "runtime_stats")
            target_compile_definitions(sample_${sample} PRIVATE RTOS_ENABLE_RUNTIME_STATS=1)
        endif()
    endforeach()

    # ---------------------------------------------------------------------------
    # trace_demo — only built when the chrome trace backend is selected.
    # ---------------------------------------------------------------------------
    if(RTOS_TRACE_BACKEND STREQUAL "chrome")
        add_executable(sample_trace_demo
                       samples/trace_demo.c
                       ${RTOS_KERNEL_SOURCES}
                       port/host/port.c)
        target_include_directories(sample_trace_demo PRIVATE include src)
        target_compile_definitions(sample_trace_demo PRIVATE
            RTOS_ENABLE_TRACE=1
            RTOS_TRACE_BACKEND_CHROME=1)
        if(MSVC)
            target_compile_options(sample_trace_demo PRIVATE /W3)
        else()
            target_compile_options(sample_trace_demo PRIVATE -fno-builtin)
        endif()
    endif()

    # ---------------------------------------------------------------------------
    # Smoke tests — self-terminating copies of host samples for ctest.
    # Each is compiled with RTOS_HOST_STOP_AFTER_TICKS so port_cpu_idle() calls
    # exit(0) once the tick limit is reached.  PASS_REGULAR_EXPRESSION verifies
    # that the sample produced the expected output before stopping.
    # Excluded on MSVC: port/host/port.c uses POSIX ucontext (unavailable on Windows).
    # ---------------------------------------------------------------------------
    if(NOT MSVC)
        set(SMOKE_TICKS_DEFAULT 700)

        foreach(sample blink producer_consumer event_group_demo)
            add_executable(smoke_${sample} samples/${sample}.c
                           ${RTOS_KERNEL_SOURCES}
                           port/host/port.c)
            target_include_directories(smoke_${sample} PRIVATE include src)
            target_compile_definitions(smoke_${sample} PRIVATE
                RTOS_HOST_STOP_AFTER_TICKS=${SMOKE_TICKS_DEFAULT})
            target_compile_options(smoke_${sample} PRIVATE -fno-builtin)
            add_test(NAME smoke_${sample} COMMAND smoke_${sample})
            set_tests_properties(smoke_${sample} PROPERTIES TIMEOUT 30)
        endforeach()

        # timer_demo prints every 1000 ticks — needs a higher limit
        add_executable(smoke_timer_demo samples/timer_demo.c
                       ${RTOS_KERNEL_SOURCES}
                       port/host/port.c)
        target_include_directories(smoke_timer_demo PRIVATE include src)
        target_compile_definitions(smoke_timer_demo PRIVATE
            RTOS_HOST_STOP_AFTER_TICKS=1200)
        target_compile_options(smoke_timer_demo PRIVATE -fno-builtin)
        add_test(NAME smoke_timer_demo COMMAND smoke_timer_demo)
        set_tests_properties(smoke_timer_demo PROPERTIES TIMEOUT 30)

        # runtime_stats prints every 5000 ticks so needs a higher limit
        add_executable(smoke_runtime_stats samples/runtime_stats.c
                       ${RTOS_KERNEL_SOURCES}
                       port/host/port.c)
        target_include_directories(smoke_runtime_stats PRIVATE include src)
        target_compile_definitions(smoke_runtime_stats PRIVATE
            RTOS_ENABLE_RUNTIME_STATS=1
            RTOS_HOST_STOP_AFTER_TICKS=5500)
        target_compile_options(smoke_runtime_stats PRIVATE -fno-builtin)
        add_test(NAME smoke_runtime_stats COMMAND smoke_runtime_stats)
        set_tests_properties(smoke_runtime_stats PROPERTIES TIMEOUT 60)

        set_tests_properties(smoke_blink             PROPERTIES PASS_REGULAR_EXPRESSION "LED")
        set_tests_properties(smoke_producer_consumer PROPERTIES PASS_REGULAR_EXPRESSION "received:")
        set_tests_properties(smoke_timer_demo        PROPERTIES PASS_REGULAR_EXPRESSION "\\[timer\\]")
        set_tests_properties(smoke_event_group_demo  PROPERTIES PASS_REGULAR_EXPRESSION "sensor ready|processing!")
        set_tests_properties(smoke_runtime_stats     PROPERTIES PASS_REGULAR_EXPRESSION "CPU%|Task")
    endif() # NOT MSVC

endif()

# ---------------------------------------------------------------------------
# Pico SDK samples (RP2040 / Pico W)
# ---------------------------------------------------------------------------

if(RTOS_TARGET_PICO)
    # uart_echo and adc_pipeline — work on any RP2040 or Pico W
    foreach(sample uart_echo adc_pipeline)
        add_executable(sample_${sample} samples/${sample}.c)
        target_link_libraries(sample_${sample} quartz pico_stdlib hardware_adc)
        target_include_directories(sample_${sample} PRIVATE include)
        if(sample STREQUAL "adc_pipeline")
            target_link_libraries(sample_${sample} m)
        endif()
        pico_add_extra_outputs(sample_${sample})
        pico_enable_stdio_usb(sample_${sample} 1)
        pico_enable_stdio_uart(sample_${sample} 0)
    endforeach()

    # timer_demo — software timer demo; no special hardware required
    add_executable(sample_timer_demo samples/timer_demo.c)
    target_link_libraries(sample_timer_demo quartz pico_stdlib)
    target_include_directories(sample_timer_demo PRIVATE include)
    pico_add_extra_outputs(sample_timer_demo)
    pico_enable_stdio_usb(sample_timer_demo 1)
    pico_enable_stdio_uart(sample_timer_demo 0)

    # runtime_stats — compiles the kernel directly so RTOS_ENABLE_RUNTIME_STATS=1
    # can be applied to all kernel sources without rebuilding the shared library.
    add_executable(sample_runtime_stats
                   samples/runtime_stats.c
                   ${RTOS_KERNEL_SOURCES}
                   port/arm_cm0plus/port.c
                   port/arm_cm0plus/port_asm.S)
    target_link_libraries(sample_runtime_stats pico_stdlib)
    target_include_directories(sample_runtime_stats PRIVATE include src)
    target_compile_definitions(sample_runtime_stats PRIVATE
        RTOS_ENABLE_RUNTIME_STATS=1
        RTOS_CPU_HZ=${RTOS_CPU_HZ})
    pico_add_extra_outputs(sample_runtime_stats)
    pico_enable_stdio_usb(sample_runtime_stats 1)
    pico_enable_stdio_uart(sample_runtime_stats 0)

    # dual_core — RP2040 dual-core AMP demo.  Compiles the kernel directly so
    # RTOS_NUM_CORES=2 can be applied to all kernel sources independently.
    add_executable(sample_dual_core
                   samples/dual_core.c
                   ${RTOS_KERNEL_SOURCES}
                   port/arm_cm0plus/port.c
                   port/arm_cm0plus/port_asm.S)
    target_link_libraries(sample_dual_core pico_stdlib pico_multicore)
    target_include_directories(sample_dual_core PRIVATE include src)
    target_compile_definitions(sample_dual_core PRIVATE
        RTOS_NUM_CORES=2
        RTOS_CPU_HZ=${RTOS_CPU_HZ})
    pico_add_extra_outputs(sample_dual_core)
    pico_enable_stdio_usb(sample_dual_core 1)
    pico_enable_stdio_uart(sample_dual_core 0)

    # pwm_fade — PWM LED fade with task suspend/resume; standard Pico only
    # (GP25 is the on-board LED on Pico; on Pico W it is wired to the CYW43).
    add_executable(sample_pwm_fade samples/pwm_fade.c)
    target_link_libraries(sample_pwm_fade quartz pico_stdlib hardware_pwm)
    target_include_directories(sample_pwm_fade PRIVATE include)
    pico_add_extra_outputs(sample_pwm_fade)
    pico_enable_stdio_usb(sample_pwm_fade 1)
    pico_enable_stdio_uart(sample_pwm_fade 0)

    # event_group_demo — fan-in demo using rtos_eventgroup_* (host + Pico)
    add_executable(sample_event_group_demo samples/event_group_demo.c)
    target_link_libraries(sample_event_group_demo quartz pico_stdlib)
    target_include_directories(sample_event_group_demo PRIVATE include)
    pico_add_extra_outputs(sample_event_group_demo)
    pico_enable_stdio_usb(sample_event_group_demo 1)
    pico_enable_stdio_uart(sample_event_group_demo 0)

    # wifi_http — Pico W only (requires CYW43 WiFi chip)
    if(PICO_BOARD STREQUAL "pico_w")
        add_executable(sample_wifi_http samples/wifi_http.c)
        target_link_libraries(sample_wifi_http
            quartz
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

    # pico_trace_demo — only built when the chrome trace backend is selected.
    # Routes printf over USB CDC so the host can capture the hex dump from
    # /dev/ttyACM0 (Linux/macOS/WSL) or COMx (Windows) — works whether you
    # flash via the Pi Debug Probe (SWD) or via BOOTSEL.
    if(RTOS_TRACE_BACKEND STREQUAL "chrome")
        add_executable(sample_pico_trace_demo samples/pico_trace_demo.c)
        target_link_libraries(sample_pico_trace_demo quartz pico_stdlib)
        target_include_directories(sample_pico_trace_demo PRIVATE include)
        pico_add_extra_outputs(sample_pico_trace_demo)
        pico_enable_stdio_usb(sample_pico_trace_demo 1)
        pico_enable_stdio_uart(sample_pico_trace_demo 0)
    endif()
endif()

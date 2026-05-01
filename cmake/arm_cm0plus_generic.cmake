# CMake toolchain file for ARM Cortex-M0+ bare-metal (generic, no Pico SDK).
#
# Usage:
#   cmake -B build_cm0plus \
#         -DRTOS_PORT=cm0plus \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/arm_cm0plus_generic.cmake
#   cmake --build build_cm0plus
#
# Targets a generic Cortex-M0+ (e.g. nRF51, SAMD21, STM32G0/L0).  For an
# RP2040-based board where you want the Pico SDK, use -DRTOS_PORT=pico
# with PICO_SDK_PATH set instead of this file.
#
# Supply a board-specific linker script and startup file in your application:
#   cmake -B build_cm0plus \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/arm_cm0plus_generic.cmake \
#         -DRTOS_LINKER_SCRIPT=path/to/device.ld \
#         -DRTOS_CPU_HZ=64000000
#
# Requires arm-none-eabi-gcc.
#   macOS:  brew install arm-none-eabi-gcc
#   Ubuntu: sudo apt install gcc-arm-none-eabi

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm_cm0plus)

set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY      arm-none-eabi-objcopy)
set(CMAKE_SIZE         arm-none-eabi-size)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CM0PLUS_FLAGS
    "-mcpu=cortex-m0plus"
    "-mthumb"
    "-mfloat-abi=soft"
    "-ffreestanding"
    "-fno-exceptions"
    "-ffunction-sections"
    "-fdata-sections"
    "-Os"
    "-Wall"
)

string(JOIN " " CM0PLUS_FLAGS_STR ${CM0PLUS_FLAGS})
set(CMAKE_C_FLAGS   "${CM0PLUS_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS "${CM0PLUS_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS
    "-mcpu=cortex-m0plus -mthumb -mfloat-abi=soft -Wl,--gc-sections -nostartfiles"
    CACHE STRING "" FORCE)

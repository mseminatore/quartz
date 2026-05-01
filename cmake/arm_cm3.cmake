# CMake toolchain file for ARM Cortex-M3 bare-metal (generic).
#
# Usage:
#   cmake -B build_cm3 \
#         -DRTOS_PORT=cm3 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/arm_cm3.cmake
#   cmake --build build_cm3
#
# Targets a generic Cortex-M3 (no FPU).  Common examples:
#   STM32F1xx, LPC17xx, NXP LPC1768.
#
# Supply a board-specific linker script and startup file for your device:
#   cmake -B build_cm3 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/arm_cm3.cmake \
#         -DRTOS_LINKER_SCRIPT=path/to/device.ld \
#         -DRTOS_CPU_HZ=72000000
#
# Requires arm-none-eabi-gcc.
#   macOS:  brew install arm-none-eabi-gcc
#   Ubuntu: sudo apt install gcc-arm-none-eabi

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm_cm3)

set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY      arm-none-eabi-objcopy)
set(CMAKE_SIZE         arm-none-eabi-size)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CM3_FLAGS
    "-mcpu=cortex-m3"
    "-mthumb"
    "-mfloat-abi=soft"
    "-ffreestanding"
    "-fno-exceptions"
    "-ffunction-sections"
    "-fdata-sections"
    "-Os"
    "-Wall"
)

string(JOIN " " CM3_FLAGS_STR ${CM3_FLAGS})
set(CMAKE_C_FLAGS   "${CM3_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS "${CM3_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS
    "-mcpu=cortex-m3 -mthumb -mfloat-abi=soft -Wl,--gc-sections -nostartfiles"
    CACHE STRING "" FORCE)

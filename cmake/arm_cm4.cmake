# CMake toolchain file for ARM Cortex-M4F bare-metal (generic).
#
# Usage:
#   cmake -B build_cm4 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/arm_cm4.cmake
#   cmake --build build_cm4
#
# Targets a generic Cortex-M4F with hardware FPU.  Common examples:
#   STM32F4xx (168 MHz), STM32F3xx (72 MHz), LPC43xx (204 MHz).
#
# Supply a board-specific linker script and startup file for your device:
#   cmake -B build_cm4 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/arm_cm4.cmake \
#         -DRTOS_LINKER_SCRIPT=path/to/device.ld \
#         -DRTOS_CPU_HZ=168000000
#
# Requires arm-none-eabi-gcc.
#   macOS:  brew install arm-none-eabi-gcc
#   Ubuntu: sudo apt install gcc-arm-none-eabi

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm_cm4)

set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY      arm-none-eabi-objcopy)
set(CMAKE_SIZE         arm-none-eabi-size)

# Prevent CMake from trying to link a test executable during configuration.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CM4_FLAGS
    "-mcpu=cortex-m4"
    "-mthumb"
    "-mfpu=fpv4-sp-d16"
    "-mfloat-abi=softfp"    # softfp: ABI-compatible with soft-float, uses HW FPU
    "-ffreestanding"
    "-fno-exceptions"
    "-ffunction-sections"
    "-fdata-sections"
    "-Os"
    "-Wall"
)

string(JOIN " " CM4_FLAGS_STR ${CM4_FLAGS})
set(CMAKE_C_FLAGS   "${CM4_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS "${CM4_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS
    "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=softfp -Wl,--gc-sections -nostartfiles"
    CACHE STRING "" FORCE)

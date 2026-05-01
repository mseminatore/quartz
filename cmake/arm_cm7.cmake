# CMake toolchain file for ARM Cortex-M7 bare-metal (generic, single-precision FPU).
#
# Usage:
#   cmake -B build_cm7 \
#         -DRTOS_PORT=cm7 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/arm_cm7.cmake
#   cmake --build build_cm7
#
# Targets a generic Cortex-M7 with single-precision FPv5 FPU.  Common examples:
#   STM32F7xx, STM32H7xx, NXP i.MX RT, Microchip SAME70.
#
# For double-precision FPU (D16-D31), change -mfpu to fpv5-d16; note the kernel
# context-switch (port/arm_cm4/port_asm.S) currently saves S16-S31 only.  If
# tasks use the upper double-precision registers (D16-D31), extend port_asm.S.
#
# Supply a board-specific linker script and startup file for your device.
#
# Requires arm-none-eabi-gcc.
#   macOS:  brew install arm-none-eabi-gcc
#   Ubuntu: sudo apt install gcc-arm-none-eabi

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm_cm7)

set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY      arm-none-eabi-objcopy)
set(CMAKE_SIZE         arm-none-eabi-size)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CM7_FLAGS
    "-mcpu=cortex-m7"
    "-mthumb"
    "-mfpu=fpv5-sp-d16"
    "-mfloat-abi=softfp"
    "-ffreestanding"
    "-fno-exceptions"
    "-ffunction-sections"
    "-fdata-sections"
    "-Os"
    "-Wall"
)

string(JOIN " " CM7_FLAGS_STR ${CM7_FLAGS})
set(CMAKE_C_FLAGS   "${CM7_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS "${CM7_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS
    "-mcpu=cortex-m7 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=softfp -Wl,--gc-sections -nostartfiles"
    CACHE STRING "" FORCE)

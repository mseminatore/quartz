# CMake toolchain file for RV32IMAC bare-metal (CLINT timer, machine mode).
#
# Usage:
#   cmake -B build_rv32 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/riscv32_clint.cmake
#   cmake --build build_rv32
#
# Targets QEMU virt machine by default.  For SiFive HiFive1 (FE310) add:
#   -DRTOS_MTIME_HZ=32768
# and supply a board-specific linker script with -DRTOS_LINKER_SCRIPT=...
#
# Requires riscv64-unknown-elf-gcc (targeting RV32 via -march=rv32imac).
# On macOS: brew install riscv-software-src/riscv/riscv-gnu-toolchain
# On Ubuntu: sudo apt install gcc-riscv64-unknown-elf

set(CMAKE_SYSTEM_NAME  Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv)

set(CMAKE_C_COMPILER   riscv64-unknown-elf-gcc)
set(CMAKE_ASM_COMPILER riscv64-unknown-elf-gcc)
set(CMAKE_OBJCOPY      riscv64-unknown-elf-objcopy)
set(CMAKE_SIZE         riscv64-unknown-elf-size)

# Prevent CMake from trying to link a test executable during configuration.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(RV32_FLAGS
    "-march=rv32imac"
    "-mabi=ilp32"
    "-mcmodel=medany"
    "-ffreestanding"
    "-fno-exceptions"
    "-ffunction-sections"
    "-fdata-sections"
    "-Os"
    "-Wall"
)

string(JOIN " " RV32_FLAGS_STR ${RV32_FLAGS})
set(CMAKE_C_FLAGS   "${RV32_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS "${RV32_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS
    "-march=rv32imac -mabi=ilp32 -Wl,--gc-sections -nostartfiles"
    CACHE STRING "" FORCE)

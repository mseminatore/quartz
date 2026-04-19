# CMake toolchain file for ESP32-S3 (Xtensa LX7), Call0 ABI, QEMU target.
#
# Usage:
#   cmake -B build_esp32s3 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/esp32s3_qemu.cmake
#   cmake --build build_esp32s3
#
#   # Run on Espressif QEMU fork:
#   qemu-system-xtensa -machine esp32s3 -nographic \
#                      -kernel build_esp32s3/rtos.elf
#
# Requires Espressif's Xtensa toolchain (xtensa-esp32s3-elf-gcc).
# Via ESP-IDF: source ~/.espressif/tools/.../activate (sets PATH).
# Standalone: https://github.com/espressif/crosstool-NG/releases
#   or: brew install espressif/homebrew-esp/esp-xtensa (macOS)
#
# Requires Espressif QEMU:
#   https://github.com/espressif/qemu/releases

set(CMAKE_SYSTEM_NAME  Generic)
set(CMAKE_SYSTEM_PROCESSOR xtensa-esp32s3)

set(CMAKE_C_COMPILER   xtensa-esp32s3-elf-gcc)
set(CMAKE_ASM_COMPILER xtensa-esp32s3-elf-gcc)
set(CMAKE_OBJCOPY      xtensa-esp32s3-elf-objcopy)
set(CMAKE_SIZE         xtensa-esp32s3-elf-size)

# Prevent CMake from trying to link a test executable during configuration.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(XTENSA_FLAGS
    "-mabi=call0"          # Call0 ABI — no register window rotation
    "-mlongcalls"          # Allow l32r for calls >±512 KB (IRAM→flash literals)
    "-mtext-section-literals" # Place literal pools in .text (needed for IRAM)
    "-Os"
    "-fno-exceptions"
    "-ffunction-sections"
    "-fdata-sections"
    "-Wall"
    "-ffreestanding"
)

string(JOIN " " XTENSA_FLAGS_STR ${XTENSA_FLAGS})
set(CMAKE_C_FLAGS   "${XTENSA_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS "${XTENSA_FLAGS_STR}" CACHE STRING "" FORCE)

# Linker script is in the port directory; resolved relative to project root.
set(XTENSA_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/port/xtensa_esp32s3/link.ld")

set(CMAKE_EXE_LINKER_FLAGS
    "-mabi=call0 -mlongcalls -nostartfiles -Wl,--gc-sections -T${XTENSA_LINKER_SCRIPT}"
    CACHE STRING "" FORCE)

# CMake toolchain file for AVR ATmega328P (avr-gcc cross-compiler).
#
# Usage:
#   cmake -B build_avr \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/avr_atmega328p.cmake \
#         [-DF_CPU=16000000UL]
#   cmake --build build_avr
#   avrdude -c arduino -p m328p -P /dev/ttyUSB0 -b 115200 \
#           -U flash:w:build_avr/rtos.hex

set(CMAKE_SYSTEM_NAME  Generic)
set(CMAKE_SYSTEM_PROCESSOR avr)

# Compiler
set(CMAKE_C_COMPILER   avr-gcc)
set(CMAKE_ASM_COMPILER avr-gcc)
set(CMAKE_OBJCOPY      avr-objcopy)
set(CMAKE_SIZE         avr-size)

# Prevent CMake from trying to link a test executable during configuration
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Device flags — ATmega328P at 16 MHz by default
if(NOT DEFINED F_CPU)
    set(F_CPU "16000000UL")
endif()

set(AVR_MCU "atmega328p")

set(AVR_FLAGS
    "-mmcu=${AVR_MCU}"
    "-DF_CPU=${F_CPU}"
    "-Os"
    "-ffunction-sections"
    "-fdata-sections"
    "-fno-exceptions"
    "-Wall"
)

string(JOIN " " AVR_FLAGS_STR ${AVR_FLAGS})
set(CMAKE_C_FLAGS   "${AVR_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS "${AVR_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS
    "-mmcu=${AVR_MCU} -Wl,--gc-sections" CACHE STRING "" FORCE)

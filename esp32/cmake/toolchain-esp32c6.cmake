# toolchain-esp32c6.cmake -- RISC-V cross-compile settings for the ESP32-C6.
#
# OPTIONAL. The authoritative build for this spike is the plain Makefile
# (`make -C esp32`); a CMake front-end (a CMakeLists.txt that consumes this
# file) is roadmap. It is provided so the tree matches the layout in README.md
# and so a future CMake build reuses one definition of the compiler and flags.
# Mirrors cmake/toolchain-ra8d2.cmake in the RA8 tree.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)

# Single HP RISC-V core: rv32imac, ilp32 ABI, little-endian.
set(ESP32C6_ARCH_FLAGS "-march=rv32imac -mabi=ilp32")

set(CMAKE_C_COMPILER riscv64-elf-gcc)
set(CMAKE_ASM_COMPILER riscv64-elf-gcc)
set(CMAKE_OBJCOPY
    riscv64-elf-objcopy
    CACHE FILEPATH "objcopy"
)
set(CMAKE_OBJDUMP
    riscv64-elf-objdump
    CACHE FILEPATH "objdump"
)

# Freestanding, no host libc/startup; the linker script + start.S own the runtime.
set(_esp32c6_c_flags
    -std=c23
    -nostdlib
    -ffreestanding
    -Os
    -Wall
    -Wextra
    -Werror
    -ffunction-sections
    -fdata-sections
)
string(JOIN " " _esp32c6_c_flags_str ${_esp32c6_c_flags})
set(CMAKE_C_FLAGS_INIT "${ESP32C6_ARCH_FLAGS} ${_esp32c6_c_flags_str}")
set(CMAKE_ASM_FLAGS_INIT "${ESP32C6_ARCH_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${ESP32C6_ARCH_FLAGS} -nostdlib -Wl,--gc-sections")

# We are cross-compiling: never run target binaries during CMake's compiler checks.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

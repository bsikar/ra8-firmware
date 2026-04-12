cmake_minimum_required(VERSION 3.20)

#
# ra8d2-firmware host-compiled unit test build.
#
# Compiles every library source file with the host (x86_64) compiler
# so clang-tidy has a valid compile_commands.json and so actual unit
# tests can link against mock peripheral implementations.
#
# No target cross-compilation happens here -- that lives in the
# top-level CMakeLists.txt.
#
# Usage from the firmware root:
#
#     cmake -B build/tidy -S tests -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
#     cmake --build build/tidy
#
# Or via:
#
#     bash scripts/clang_tidy.sh --check
#

project(ra8d2-firmware-tests
    LANGUAGES C
    VERSION 0.1.0
    DESCRIPTION "Host-compiled unit tests for ra8d2-firmware"
)

# Host C23 build -- the target uses the same standard.
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Every TU is compiled with RA_SIMULATOR_MODE so code that normally
# reaches into MCU register blocks takes the host-safe path instead.
add_compile_definitions(RA_SIMULATOR_MODE UNIT_TEST)

# Host warnings. We deliberately do NOT use -Wstack-usage=2200 here
# because the host compiler ABI pushes wider arguments than Cortex-M85.
add_compile_options(
    -Wall
    -Wextra
    -Werror
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-address-of-packed-member
)

# Firmware root (one directory up from tests/).
get_filename_component(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Collect every library source. Each .c file becomes part of the
# `ra_core_hal` static library. clang-tidy then walks the associated
# compile_commands.json.
file(GLOB_RECURSE RA_CORE_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_core/src/*.c)
file(GLOB_RECURSE RA_HAL_SOURCES  CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_hal/src/*.c)

add_library(ra_core_hal OBJECT
    ${RA_CORE_SOURCES}
    ${RA_HAL_SOURCES}
)
target_include_directories(ra_core_hal PUBLIC
    ${FW_ROOT}/libs/ra_core/inc
    ${FW_ROOT}/libs/ra_hal/inc
    ${FW_ROOT}/src
    ${FW_ROOT}/src/inc
)

# Enable ctest so `cmake --build . --target test` and bash helpers work.
enable_testing()

# TODO: real unit tests will live here as `test_ra_*.c` files. For now
# the build exists purely so compile_commands.json is generated for
# clang-tidy.

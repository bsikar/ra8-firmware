#
# Per-app CMake for the `crc_demo` firmware.
#
# Usable two ways:
#   1. Standalone, from inside `crc_demo/`:
#        cmake -S . -B build \
#              -DCMAKE_TOOLCHAIN_FILE=../../../cmake/toolchain-ra8d2.cmake
#        cmake --build build
#   2. Pulled in by the top-level CMakeLists.txt via add_subdirectory(examples/crc_demo).
#
# Either path produces crc_demo.elf / crc_demo.hex / crc_demo.bin in the
# build dir.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

# Detect standalone invocation. When this file is the top-level project,
# we set up everything (project, version, build type, std). When pulled
# in by the repo root, the parent has already done that.
get_directory_property(_ra_has_parent PARENT_DIRECTORY)
set(RA_APP_STANDALONE FALSE)
if(NOT _ra_has_parent)
    set(RA_APP_STANDALONE TRUE)
endif()

if(RA_APP_STANDALONE)
    get_filename_component(RA_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

    file(READ "${RA_REPO_ROOT}/VERSION" RA_PROJECT_VERSION)
    string(STRIP "${RA_PROJECT_VERSION}" RA_PROJECT_VERSION)

    project(crc_demo
        LANGUAGES C ASM
        VERSION ${RA_PROJECT_VERSION}
        DESCRIPTION "crc_demo firmware for RA8D2"
    )

    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    endif()

    set(CMAKE_C_STANDARD 23)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    set(CMAKE_C_FLAGS_DEBUG          "-O0 -g3 -DDEBUG")
    set(CMAKE_ASM_FLAGS_DEBUG        "-g3")
    set(CMAKE_C_FLAGS_RELEASE        "-Os -g1 -DNDEBUG")
    set(CMAKE_ASM_FLAGS_RELEASE      "-g1")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "-Og -g3")
    set(CMAKE_ASM_FLAGS_RELWITHDEBINFO "-g3")

    option(RA_TRUSTZONE_ENABLE "Enable Cortex-M85 TrustZone (-mcmse + SAU)" OFF)
else()
    get_filename_component(RA_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
endif()

include(${RA_REPO_ROOT}/cmake/ra_warnings.cmake)

#############################################################################
# Sources
#############################################################################

set(RA_APP_NAME crc_demo)

set(APP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/vector_table.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system_init.c
    ${CMAKE_CURRENT_SOURCE_DIR}/secure_exception.c
    ${CMAKE_CURRENT_SOURCE_DIR}/trustzone_init.c
)

file(GLOB_RECURSE LIB_RA_CORE_SOURCES   CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_core/src/*.c)
file(GLOB_RECURSE LIB_RA_HAL_SOURCES    CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_hal/src/*.c)
file(GLOB_RECURSE LIB_RA_NET_PAL_SOURCES CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_net_pal/src/*.c)
file(GLOB_RECURSE LIB_RA_USB_PAL_SOURCES CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_usb_pal/src/*.c)
file(GLOB_RECURSE LIB_RA_NSC_SOURCES    CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_nsc/src/*.c)
file(GLOB_RECURSE LIB_RA_BOARD_SOURCES  CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_board_ek_ra8d2/src/*.c)
file(GLOB_RECURSE APP_SECURE_SOURCES    CONFIGURE_DEPENDS ${RA_REPO_ROOT}/src/secure_app/*.c)

#############################################################################
# Executable
#############################################################################

set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld)

add_executable(${RA_APP_NAME}.elf
    ${APP_SOURCES}
    ${LIB_RA_CORE_SOURCES}
    ${LIB_RA_HAL_SOURCES}
    ${LIB_RA_NET_PAL_SOURCES}
    ${LIB_RA_USB_PAL_SOURCES}
    ${LIB_RA_NSC_SOURCES}
    ${LIB_RA_BOARD_SOURCES}
    ${APP_SECURE_SOURCES}
)

ra_target_enable_project_warnings(${RA_APP_NAME}.elf STACK_USAGE_BYTES 2200)
target_compile_options(${RA_APP_NAME}.elf PRIVATE -fshort-enums)

if(RA_TRUSTZONE_ENABLE)
    target_compile_definitions(${RA_APP_NAME}.elf PRIVATE RA_TRUSTZONE_ENABLE)
    target_compile_options(${RA_APP_NAME}.elf PRIVATE -mcmse)
    target_link_options(${RA_APP_NAME}.elf PRIVATE -mcmse)
endif()

target_include_directories(${RA_APP_NAME}.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RA_REPO_ROOT}/src
    ${RA_REPO_ROOT}/src/inc
    ${RA_REPO_ROOT}/src/secure_app
    ${RA_REPO_ROOT}/libs/ra_core/inc
    ${RA_REPO_ROOT}/libs/ra_hal/inc
    ${RA_REPO_ROOT}/libs/ra_net_pal/inc
    ${RA_REPO_ROOT}/libs/ra_usb_pal/inc
    ${RA_REPO_ROOT}/libs/ra_nsc/inc
    ${RA_REPO_ROOT}/libs/ra_board_ek_ra8d2/inc
)

target_link_options(${RA_APP_NAME}.elf PRIVATE
    -T${LINKER_SCRIPT}
    -Wl,--Map=${RA_APP_NAME}.map
)

set_target_properties(${RA_APP_NAME}.elf PROPERTIES LINK_DEPENDS ${LINKER_SCRIPT})

#############################################################################
# Post-build: .hex / .bin / size dump
#############################################################################

add_custom_command(TARGET ${RA_APP_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${RA_APP_NAME}.elf> ${RA_APP_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${RA_APP_NAME}.elf> ${RA_APP_NAME}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${RA_APP_NAME}.elf>
    COMMENT "Generating ${RA_APP_NAME}.hex / .bin and showing size"
)

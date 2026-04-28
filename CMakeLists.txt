#
# Top-level CMake for ra8d2-firmware (Cortex-M85 cross-build).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

# Project version is owned by the top-level VERSION file so a single
# `echo 0.2.0 > VERSION` propagates to cmake, the firmware @since tags,
# and the pre-commit version-consistency check at the same time.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RA_PROJECT_VERSION)
string(STRIP "${RA_PROJECT_VERSION}" RA_PROJECT_VERSION)

project(ra8d2-firmware
    LANGUAGES C ASM
    VERSION ${RA_PROJECT_VERSION}
    DESCRIPTION "Bare-metal firmware for Renesas RA8D2 (Cortex-M85) on EK-RA8D2"
)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler: ${CMAKE_C_COMPILER}")

# C Standard - C23 (GNU2x)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # Enable GNU extensions

# Export compile commands for clangd/tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#############################################################################
# Compiler Flags (per build type)
#############################################################################

# Debug flags: no optimization, full debug info
set(CMAKE_C_FLAGS_DEBUG "-O0 -g3 -DDEBUG")
set(CMAKE_ASM_FLAGS_DEBUG "-g3")

# Release flags: size-optimized, minimal debug info
set(CMAKE_C_FLAGS_RELEASE "-Os -g1 -DNDEBUG")
set(CMAKE_ASM_FLAGS_RELEASE "-g1")

# RelWithDebInfo: -Og for a debuggable optimized build
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-Og -g3")
set(CMAKE_ASM_FLAGS_RELWITHDEBINFO "-g3")

#############################################################################
# Source Files
#############################################################################

# Application entry point comes from examples/<EXAMPLE>/. Each example
# is a standalone main.c that builds against the same boot code, libs,
# and secure_app substrate. Select with `-DEXAMPLE=<name>`; default is
# "blink".
set(EXAMPLE "blink" CACHE STRING "Example app to build (subdir of examples/)")
set(EXAMPLE_DIR ${CMAKE_SOURCE_DIR}/examples/${EXAMPLE})
if(NOT EXISTS ${EXAMPLE_DIR}/main.c)
    message(FATAL_ERROR "Example '${EXAMPLE}' not found at ${EXAMPLE_DIR}/main.c.")
endif()
message(STATUS "Example:       ${EXAMPLE} (${EXAMPLE_DIR}/main.c)")

file(GLOB APP_SOURCES_ROOT CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.c
    ${EXAMPLE_DIR}/*.c
)
file(GLOB_RECURSE APP_BOOT_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/boot/*.c
    ${CMAKE_SOURCE_DIR}/src/boot/*.S
)
file(GLOB_RECURSE LIB_RA_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/libs/ra_core/src/*.c
)
file(GLOB_RECURSE LIB_RA_HAL_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/libs/ra_hal/src/*.c
)
file(GLOB_RECURSE LIB_RA_NET_PAL_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/libs/ra_net_pal/src/*.c
)
file(GLOB_RECURSE LIB_RA_USB_PAL_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/libs/ra_usb_pal/src/*.c
)
file(GLOB_RECURSE LIB_RA_NSC_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/libs/ra_nsc/src/*.c
)
file(GLOB_RECURSE APP_SECURE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/secure_app/*.c
)

set(ALL_SOURCES
    ${APP_SOURCES_ROOT}
    ${APP_BOOT_SOURCES}
    ${LIB_RA_CORE_SOURCES}
    ${LIB_RA_HAL_SOURCES}
    ${LIB_RA_NET_PAL_SOURCES}
    ${LIB_RA_USB_PAL_SOURCES}
    ${LIB_RA_NSC_SOURCES}
    ${APP_SECURE_SOURCES}
)

#############################################################################
# Include Directories
#############################################################################

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/secure_app
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/ra_core/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/ra_hal/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/ra_net_pal/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/ra_usb_pal/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/ra_nsc/inc
)

#############################################################################
# Build options
#############################################################################

# TrustZone bring-up. OFF by default so the single-world
# build (..8) keeps working unchanged. When ON, the SAU is
# programmed in SystemInit and the toolchain receives -mcmse so
# the NSC veneers can carry __cmse_nonsecure_entry attributes.
option(RA_TRUSTZONE_ENABLE "Enable Cortex-M85 TrustZone (-mcmse + SAU)" OFF)

#############################################################################
# Executable Target
#############################################################################

# Find linker script (TODO: write src/linker_script.ld for RA8D2 memory map)
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/src/linker_script.ld)

if(ALL_SOURCES)
    add_executable(${PROJECT_NAME}.elf ${ALL_SOURCES})

    target_compile_options(${PROJECT_NAME}.elf PRIVATE
        -Wall
        -Wextra
        -Werror
        -fshort-enums
        $<$<COMPILE_LANGUAGE:C>:-Wstack-usage=2200>
    )

    if(RA_TRUSTZONE_ENABLE)
        target_compile_definitions(${PROJECT_NAME}.elf PRIVATE RA_TRUSTZONE_ENABLE)
        target_compile_options(${PROJECT_NAME}.elf PRIVATE -mcmse)
        target_link_options(${PROJECT_NAME}.elf PRIVATE -mcmse)
    endif()

    target_compile_definitions(${PROJECT_NAME}.elf PRIVATE
        # Chip-specific defines will go here once we start coding
    )

    target_include_directories(${PROJECT_NAME}.elf PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/inc
        ${CMAKE_SOURCE_DIR}/src/secure_app
        ${CMAKE_SOURCE_DIR}/libs/ra_core/inc
        ${CMAKE_SOURCE_DIR}/libs/ra_hal/inc
        ${CMAKE_SOURCE_DIR}/libs/ra_net_pal/inc
        ${CMAKE_SOURCE_DIR}/libs/ra_usb_pal/inc
        ${CMAKE_SOURCE_DIR}/libs/ra_nsc/inc
    )

    if(EXISTS ${LINKER_SCRIPT})
        target_link_options(${PROJECT_NAME}.elf PRIVATE
            -T${LINKER_SCRIPT}
            -Wl,--Map=${PROJECT_NAME}.map
        )
    else()
        message(WARNING "Linker script not found at ${LINKER_SCRIPT} -- link step will fail until written")
    endif()

    #############################################################################
    # Post-build: Generate .hex and .bin from .elf
    #############################################################################
    add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}.elf>
        COMMENT "Generating ${PROJECT_NAME}.hex / .bin and showing size"
    )
else()
    message(STATUS "ALL_SOURCES is empty -- no executable target defined yet. "
                   "Add source files to src/ or libs/ and uncomment the globs above.")
endif()

#############################################################################
# Testing Infrastructure
#############################################################################

enable_testing()

#############################################################################
# Print Summary
#############################################################################

message(STATUS "====================================")
message(STATUS "ra8d2-firmware Build Configuration")
message(STATUS "====================================")
message(STATUS "Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C Standard: C${CMAKE_C_STANDARD} (GNU extensions enabled)")
message(STATUS "Linker Script: ${LINKER_SCRIPT}")
message(STATUS "Source Root: ${CMAKE_CURRENT_LIST_DIR}")
message(STATUS "====================================")

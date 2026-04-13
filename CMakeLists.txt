#
# Top-level CMake for ra8d2-firmware (Cortex-M85 cross-build).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

project(ra8d2-firmware
    LANGUAGES C ASM
    VERSION 0.1.0
    DESCRIPTION "Bare-metal firmware for Renesas RA8D2 (Cortex-M85) on EK-RA8D2"
)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler:   ${CMAKE_C_COMPILER}")

# C Standard - C23 (GNU2x)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)  # Enable GNU extensions

# Export compile commands for clangd/tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#############################################################################
# Compiler Flags (per build type)
#############################################################################

# Debug flags: no optimization, full debug info
set(CMAKE_C_FLAGS_DEBUG   "-O0 -g3 -DDEBUG")
set(CMAKE_ASM_FLAGS_DEBUG "-g3")

# Release flags: size-optimized, minimal debug info
set(CMAKE_C_FLAGS_RELEASE   "-Os -g1 -DNDEBUG")
set(CMAKE_ASM_FLAGS_RELEASE "-g1")

# RelWithDebInfo: -Og for a debuggable optimized build
set(CMAKE_C_FLAGS_RELWITHDEBINFO   "-Og -g3")
set(CMAKE_ASM_FLAGS_RELWITHDEBINFO "-g3")

#############################################################################
# Source Files
#############################################################################

file(GLOB APP_SOURCES_ROOT CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.c)
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

set(ALL_SOURCES
    ${APP_SOURCES_ROOT}
    ${APP_BOOT_SOURCES}
    ${LIB_RA_CORE_SOURCES}
    ${LIB_RA_HAL_SOURCES}
    ${LIB_RA_NET_PAL_SOURCES}
    ${LIB_RA_USB_PAL_SOURCES}
)

#############################################################################
# Include Directories
#############################################################################

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/ra_core/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/ra_hal/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/ra_net_pal/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/ra_usb_pal/inc
)

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

    target_compile_definitions(${PROJECT_NAME}.elf PRIVATE
        # Chip-specific defines will go here once we start coding
    )

    target_include_directories(${PROJECT_NAME}.elf PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/inc
        ${CMAKE_SOURCE_DIR}/libs/ra_core/inc
        ${CMAKE_SOURCE_DIR}/libs/ra_hal/inc
        ${CMAKE_SOURCE_DIR}/libs/ra_net_pal/inc
        ${CMAKE_SOURCE_DIR}/libs/ra_usb_pal/inc
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
        COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
        COMMAND ${CMAKE_SIZE}              $<TARGET_FILE:${PROJECT_NAME}.elf>
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
message(STATUS "Compiler:      ${CMAKE_C_COMPILER}")
message(STATUS "Build Type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "C Standard:    C${CMAKE_C_STANDARD} (GNU extensions enabled)")
message(STATUS "Linker Script: ${LINKER_SCRIPT}")
message(STATUS "Source Root:   ${CMAKE_CURRENT_LIST_DIR}")
message(STATUS "====================================")

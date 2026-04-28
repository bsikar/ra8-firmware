#
# Top-level CMake for ra8d2-firmware (Cortex-M85 cross-build).
#
# Each top-level directory containing a `main.c` is treated as a
# standalone application -- its own boot files, linker script, and
# CMakeLists.txt. The top-level project pulls them all in via
# add_subdirectory() so a single `cmake --build build` builds every
# app in one shot.
#
# Per-app `make` from inside `<app>/` is the intended day-to-day
# entry point; this top-level cmake exists so CI can build everything
# with one configure step.
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
message(STATUS "Compiler:   ${CMAKE_C_COMPILER}")

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
set(CMAKE_C_FLAGS_DEBUG          "-O0 -g3 -DDEBUG")
set(CMAKE_ASM_FLAGS_DEBUG        "-g3")

# Release flags: size-optimized, minimal debug info
set(CMAKE_C_FLAGS_RELEASE        "-Os -g1 -DNDEBUG")
set(CMAKE_ASM_FLAGS_RELEASE      "-g1")

# RelWithDebInfo: -Og for a debuggable optimized build
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-Og -g3")
set(CMAKE_ASM_FLAGS_RELWITHDEBINFO "-g3")

#############################################################################
# Build options
#############################################################################

# TrustZone bring-up. OFF by default so the single-world build keeps
# working unchanged. When ON, the SAU is programmed in SystemInit and
# the toolchain receives -mcmse so the NSC veneers can carry
# __cmse_nonsecure_entry attributes.
option(RA_TRUSTZONE_ENABLE "Enable Cortex-M85 TrustZone (-mcmse + SAU)" OFF)

# Default app for top-level builds. `cmake --build build --target firmware`
# builds the default; `cmake --build build` builds every discovered app.
set(RA_DEFAULT_APP "blink" CACHE STRING "Default app target alias")

#############################################################################
# Auto-discover apps: any top-level dir containing main.c is an app
#############################################################################

set(RA_APPS "")
file(GLOB _ra_top_entries RELATIVE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/*)
foreach(entry ${_ra_top_entries})
    set(full ${CMAKE_SOURCE_DIR}/${entry})
    if(IS_DIRECTORY ${full}
       AND EXISTS ${full}/main.c
       AND EXISTS ${full}/CMakeLists.txt)
        list(APPEND RA_APPS ${entry})
    endif()
endforeach()

list(SORT RA_APPS)

if(NOT RA_APPS)
    message(FATAL_ERROR "No apps discovered (looking for top-level dirs with main.c + CMakeLists.txt).")
endif()

message(STATUS "Discovered apps: ${RA_APPS}")

foreach(app ${RA_APPS})
    add_subdirectory(${app})
endforeach()

# Convenience alias `firmware` that maps to the default app's elf.
if("${RA_DEFAULT_APP}" IN_LIST RA_APPS)
    add_custom_target(firmware DEPENDS ${RA_DEFAULT_APP}.elf)
    message(STATUS "Default app: ${RA_DEFAULT_APP} (alias 'firmware')")
else()
    message(WARNING "RA_DEFAULT_APP='${RA_DEFAULT_APP}' is not among discovered apps.")
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
message(STATUS "Compiler:   ${CMAKE_C_COMPILER}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C Standard: C${CMAKE_C_STANDARD} (GNU extensions enabled)")
message(STATUS "Apps:       ${RA_APPS}")
message(STATUS "Default:    ${RA_DEFAULT_APP}")
message(STATUS "Source Root: ${CMAKE_CURRENT_LIST_DIR}")
message(STATUS "====================================")

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Top-level CMake for ra8-firmware (Cortex-M85 cross-build).
#
# Every directory under `examples/` containing a `main.c` is treated
# as a standalone application -- its own boot files, linker script,
# and CMakeLists.txt. The top-level project pulls them all in via
# add_subdirectory() so a single `cmake --build build` builds every
# app in one shot.
#
# Per-app `make` from inside `examples/<app>/` is the intended
# day-to-day entry point; this top-level cmake exists so CI can build
# everything with one configure step.
#
#

cmake_minimum_required(VERSION 3.20)

# Project version is owned by the top-level VERSION file so a single
# `echo 0.2.0 > VERSION` propagates to cmake, the firmware @since tags,
# and the pre-commit version-consistency check at the same time.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RA8_PROJECT_VERSION)
string(STRIP "${RA8_PROJECT_VERSION}" RA8_PROJECT_VERSION)

project(
  ra8-firmware
  LANGUAGES C CXX ASM
  VERSION ${RA8_PROJECT_VERSION}
  DESCRIPTION "Bare-metal firmware for Renesas RA8D2 (Cortex-M85) on EK-RA8D2"
)

# Build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE
      Debug
      CACHE STRING "Build type" FORCE
  )
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler:   ${CMAKE_C_COMPILER}")

# Activate the tracked git hooks (scripts/git/) for every clone so the
# commit-msg / pre-commit / pre-push gates run for everyone -- defense in depth
# with CI. A fresh clone has no hooks installed; point core.hooksPath at the
# tracked dir at configure time. Idempotent; no-op outside a git work tree
# (.git absent in a release tarball / vendored copy).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  execute_process(
    COMMAND git config core.hooksPath scripts/git
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE _ra8_hooks_rc
    OUTPUT_QUIET ERROR_QUIET
  )
  if(_ra8_hooks_rc EQUAL 0)
    message(STATUS "Git hooks: core.hooksPath -> scripts/git")
  endif()
endif()

# Compiler cache for the host-side targets configured from this listfile
# (ra8_emulator, tools/). The cross builds get it from cmake/toolchain-ra8d2.cmake
# and the host unit tests from tests/CMakeLists.txt -- see cmake/ccache.cmake
# for why all three inclusion points are needed.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ccache.cmake")

# C Standard - C23 (GNU2x)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # Enable GNU extensions

# Export compile commands for clangd/tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ra8_warnings.cmake)

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
# Build options
#############################################################################

# TrustZone bring-up. OFF by default so the single-world build keeps
# working unchanged. When ON, the SAU is programmed in SystemInit and
# the toolchain receives -mcmse so the NSC veneers can carry
# __cmse_nonsecure_entry attributes.
option(RA8_TRUSTZONE_ENABLE "Enable Cortex-M85 TrustZone (-mcmse + SAU)" OFF)

# Vendored OS / FS integrations. OFF by default so the bare-metal-only
# top-level build keeps working without dragging the third_party trees
# in. Per-app builds (e.g. examples/threadx_filex_demo) flip these on
# explicitly through their own Makefile.
option(RA8_USE_THREADX "Enable the vendored ThreadX RTOS" OFF)
option(RA8_USE_FILEX "Enable the vendored FileX FAT file system" OFF)

# Default app for top-level builds. `cmake --build build --target firmware`
# builds the default; `cmake --build build` builds every discovered app.
set(RA8_DEFAULT_APP
    "ra8d2-ereader"
    CACHE STRING "Default app target alias"
)

# Pull in the optional integrations early so per-app subdirectories
# can reference the `threadx` / `filex` interface targets directly.
# Both files are no-ops when their option is OFF.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/threadx.cmake OPTIONAL)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/filex.cmake)

#############################################################################
# Auto-discover apps up to 4 levels deep:
#   examples/<tier>/<app>/
#   examples/<tier>/<subtier>/<app>/
#   examples/<tier>/<subtier>/<sub2>/<app>/
#
# ek_ra8d2/hw_validated/ is further split into uart/, smoke/, and manual/
# based on HIL testability; this 4-level structure is transparent to CMake.
# The app directory name (e.g. "blink") is the build-target regardless of
# depth.
#############################################################################

set(RA8_APPS "")
set(RA8_APP_PATHS "")
file(
  GLOB _ra8_tier_entries
  RELATIVE ${CMAKE_SOURCE_DIR}/examples
  ${CMAKE_SOURCE_DIR}/examples/*
)
foreach(tier ${_ra8_tier_entries})
  set(_ra8_tier_full ${CMAKE_SOURCE_DIR}/examples/${tier})
  if(NOT IS_DIRECTORY ${_ra8_tier_full})
    continue()
  endif()
  # examples/host/* are macOS-only dev tools (AppKit window backend), not
  # part of the cross-compiled firmware -- skip them in the unified build.
  #
  # examples/ra8p1_foundation/* target a DIFFERENT chip (RA8P1, R7KA8P1KFLCAC)
  # and must be configured with cmake/toolchain-ra8p1.cmake, not the RA8D2
  # toolchain this unified build uses. Skipping the tier here keeps every RA8D2
  # target (make blink, make blink_hal, ...) byte-for-behaviour unchanged; the
  # RA8P1 apps build standalone via their own per-app Makefile. See
  # libs/ra8_core/inc/ra8_device.h for the device-selection seam.
  if(tier STREQUAL "host" OR tier STREQUAL "ra8p1_foundation")
    continue()
  endif()
  file(
    GLOB _ra8_level2_entries
    RELATIVE ${_ra8_tier_full}
    ${_ra8_tier_full}/*
  )
  foreach(l2 ${_ra8_level2_entries})
    set(_ra8_l2_full ${_ra8_tier_full}/${l2})
    if(NOT IS_DIRECTORY ${_ra8_l2_full})
      continue()
    endif()
    if(EXISTS ${_ra8_l2_full}/main.c AND EXISTS ${_ra8_l2_full}/CMakeLists.txt)
      # Level 2: examples/<tier>/<app>/
      list(APPEND RA8_APPS ${l2})
      list(APPEND RA8_APP_PATHS examples/${tier}/${l2})
    else()
      file(
        GLOB _ra8_level3_entries
        RELATIVE ${_ra8_l2_full}
        ${_ra8_l2_full}/*
      )
      foreach(l3 ${_ra8_level3_entries})
        set(_ra8_l3_full ${_ra8_l2_full}/${l3})
        if(NOT IS_DIRECTORY ${_ra8_l3_full})
          continue()
        endif()
        if(EXISTS ${_ra8_l3_full}/main.c AND EXISTS ${_ra8_l3_full}/CMakeLists.txt)
          # Level 3: examples/<tier>/<subtier>/<app>/
          list(APPEND RA8_APPS ${l3})
          list(APPEND RA8_APP_PATHS examples/${tier}/${l2}/${l3})
        else()
          file(
            GLOB _ra8_level4_entries
            RELATIVE ${_ra8_l3_full}
            ${_ra8_l3_full}/*
          )
          foreach(l4 ${_ra8_level4_entries})
            set(_ra8_l4_full ${_ra8_l3_full}/${l4})
            if(IS_DIRECTORY ${_ra8_l4_full}
               AND EXISTS ${_ra8_l4_full}/main.c
               AND EXISTS ${_ra8_l4_full}/CMakeLists.txt
            )
              # Level 4: examples/<tier>/<subtier>/<sub2>/<app>/
              list(APPEND RA8_APPS ${l4})
              list(APPEND RA8_APP_PATHS examples/${tier}/${l2}/${l3}/${l4})
            endif()
          endforeach()
        endif()
      endforeach()
    endif()
  endforeach()
endforeach()

if(NOT RA8_APPS)
  message(FATAL_ERROR "No apps discovered under examples/ (checked up to 4 levels deep).")
endif()

message(STATUS "Discovered apps: ${RA8_APPS}")

foreach(app_path ${RA8_APP_PATHS})
  add_subdirectory(${app_path})
endforeach()

# Include the main e-reader application target
add_subdirectory(src/app)
list(APPEND RA8_APPS "ra8d2-ereader")

# Convenience alias `firmware` that maps to the default app's elf.
if("${RA8_DEFAULT_APP}" IN_LIST RA8_APPS)
  add_custom_target(
    firmware
    DEPENDS ${RA8_DEFAULT_APP}.elf
    COMMENT "Building the default app (${RA8_DEFAULT_APP})"
  )
  message(STATUS "Default app: ${RA8_DEFAULT_APP} (alias 'firmware')")
else()
  message(WARNING "RA8_DEFAULT_APP='${RA8_DEFAULT_APP}' is not among discovered apps.")
endif()

#############################################################################
# Testing Infrastructure
#############################################################################

enable_testing()

#############################################################################
# Print Summary
#############################################################################

message(STATUS "====================================")
message(STATUS "ra8-firmware Build Configuration")
message(STATUS "====================================")
message(STATUS "Compiler:   ${CMAKE_C_COMPILER}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C Standard: C${CMAKE_C_STANDARD} (GNU extensions enabled)")
message(STATUS "Apps:       ${RA8_APPS}")
message(STATUS "Default:    ${RA8_DEFAULT_APP}")
message(STATUS "Source Root: ${CMAKE_CURRENT_LIST_DIR}")
message(STATUS "====================================")

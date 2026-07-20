#
# cmake/threadx.cmake
#
# Top-level integration for Eclipse ThreadX on the RA8 family (Cortex-M85).
#
# Usage from a per-app CMakeLists.txt:
#
#     option(RA8_USE_THREADX "Link Eclipse ThreadX into this app" OFF)
#     if(RA8_USE_THREADX)
#         include(${RA8_REPO_ROOT}/cmake/threadx.cmake)
#         target_link_libraries(<app>.elf PRIVATE threadx)
#     endif()
#
# What it does:
#
#   - Compiles every C source under
#     libs/third_party/threadx/common/src/*.c into a static library
#     named `threadx`.
#   - Compiles every .S file under the upstream M85/GNU port
#     EXCEPT `tx_initialize_low_level.S`, which we replace with the
#     project-tuned version under `port/threadx/src/cortex_m85/`.
#   - Forces `TX_INCLUDE_USER_DEFINE_FILE` so ThreadX picks up
#     `port/threadx/inc/tx_user.h` for the firmware's tick rate, stack
#     sizes, and feature flags.
#   - Exposes the public include dirs through PUBLIC includes on the
#     `threadx` target, so any consumer that links against it can
#     `#include "tx_api.h"` directly.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

if(TARGET threadx)
  # Already configured by an earlier include in the same configure
  # pass (CMake re-includes cmake files when multiple apps add the
  # subdirectory). Nothing to do.
  return()
endif()

# When the top-level CMakeLists.txt pulls this file in via
# `include(... OPTIONAL)` -- before any per-app CMakeLists has
# explicitly opted in -- skip the build unless `RA8_USE_THREADX` is ON.
# Per-app builds set the option to ON via `-DRA8_USE_THREADX=ON` before
# they `include(.../threadx.cmake)`, so they fall through to the
# library configuration below.
if(DEFINED RA8_USE_THREADX AND NOT RA8_USE_THREADX)
  return()
endif()

if(NOT DEFINED RA8_REPO_ROOT)
  get_filename_component(RA8_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(RA8_THREADX_ROOT "${RA8_REPO_ROOT}/libs/third_party/threadx")
set(RA8_THREADX_PORT_DIR "${RA8_REPO_ROOT}/port/threadx")
set(RA8_THREADX_M85_GNU "${RA8_THREADX_ROOT}/ports/cortex_m85/gnu")

if(NOT IS_DIRECTORY "${RA8_THREADX_ROOT}")
  message(FATAL_ERROR "ThreadX vendor tree not found at ${RA8_THREADX_ROOT}")
endif()
if(NOT IS_DIRECTORY "${RA8_THREADX_M85_GNU}")
  message(FATAL_ERROR "ThreadX Cortex-M85 GNU port not found at ${RA8_THREADX_M85_GNU}")
endif()

# ---------------------------------------------------------------------------
# Common ThreadX C sources (the entire kernel).
# ---------------------------------------------------------------------------
file(GLOB RA8_THREADX_COMMON_SOURCES CONFIGURE_DEPENDS "${RA8_THREADX_ROOT}/common/src/*.c")

# ---------------------------------------------------------------------------
# Cortex-M85 GNU port sources.
#
# We pull every .S and .c file under ports/cortex_m85/gnu/src/ EXCEPT
# `tx_initialize_low_level.S` -- that one is replaced by our own under
# port/threadx/src/cortex_m85/.
# ---------------------------------------------------------------------------
file(GLOB RA8_THREADX_PORT_ASM CONFIGURE_DEPENDS "${RA8_THREADX_M85_GNU}/src/*.S")
file(GLOB RA8_THREADX_PORT_C CONFIGURE_DEPENDS "${RA8_THREADX_M85_GNU}/src/*.c")

# Drop the upstream low-level init (we replace it below).
list(
  FILTER
  RA8_THREADX_PORT_ASM
  EXCLUDE
  REGEX
  ".*/tx_initialize_low_level\\.S$"
)

# Project-tuned low-level init.
set(RA8_THREADX_PROJECT_LOW_LEVEL
    "${RA8_THREADX_PORT_DIR}/src/cortex_m85/tx_initialize_low_level.S"
)

if(NOT EXISTS "${RA8_THREADX_PROJECT_LOW_LEVEL}")
  message(FATAL_ERROR "Missing project tx_initialize_low_level.S at "
                      "${RA8_THREADX_PROJECT_LOW_LEVEL}"
  )
endif()

# ---------------------------------------------------------------------------
# Build the static library.
# ---------------------------------------------------------------------------
add_library(
  threadx STATIC
  ${RA8_THREADX_COMMON_SOURCES}
  ${RA8_THREADX_PORT_ASM}
  ${RA8_THREADX_PORT_C}
  ${RA8_THREADX_PROJECT_LOW_LEVEL}
  "${RA8_THREADX_PORT_DIR}/src/cortex_m85/tx_systick_ready.c"
  "${RA8_THREADX_PORT_DIR}/src/cortex_m85/tx_systick_retune.c"
)

# tx_systick_retune.c reprograms SysTick from the live CGC clock, so it
# needs the ra8_core / ra8_hal public headers (ra8_err.h, ra8_check.h,
# ra8_log.h, ra8_cgc.h). PRIVATE: only this library's own TUs need them;
# the symbols it references (ra8_cgc_get_clock_hz, ra8_log_*) resolve at
# final-app link time against ra8_hal / ra8_core, which every ThreadX app
# already links.
target_include_directories(
  threadx PRIVATE "${RA8_REPO_ROOT}/libs/ra8_core/inc" "${RA8_REPO_ROOT}/libs/ra8_hal/inc"
)

# Vendor headers + project tx_user.h. Public so app TUs can #include
# "tx_api.h" without re-stating the include dirs.
target_include_directories(
  threadx SYSTEM PUBLIC "${RA8_THREADX_ROOT}/common/inc" "${RA8_THREADX_M85_GNU}/inc"
)
target_include_directories(threadx PUBLIC "${RA8_THREADX_PORT_DIR}/inc")

# Force ThreadX to pick up our tx_user.h on every TU it compiles, and
# also expose the same define to consumers so they get the same view of
# kernel options when they include <tx_api.h>.
target_compile_definitions(threadx PUBLIC TX_INCLUDE_USER_DEFINE_FILE)

# Force-pull `_tx_timer_interrupt` out of libthreadx.a even when no
# strong reference exists in the app's directly-compiled .obj files.
# The shared SysTick_Handler in libs/ra8_core/src/ra8_time.c only takes a
# WEAK reference to it (so non-ThreadX apps still link), and that weak
# reference is satisfied with NULL if nothing else pulls the symbol in.
# Without this --undefined the ThreadX time base never advances when an
# app uses ThreadX via the port-level SysTick path -- Issue #8.
target_link_options(
  threadx INTERFACE -Wl,--undefined=_tx_timer_interrupt -Wl,--undefined=g_ra8_threadx_systick_ready
)

# Quiet the upstream sources -- they trigger a handful of warnings that
# the firmware build elevates to errors. Apply only to C TUs; the .S
# files are passed through the assembler and reject -W flags.
target_compile_options(threadx PRIVATE $<$<COMPILE_LANGUAGE:C>:-w>)

message(STATUS "ThreadX: ${CMAKE_PROJECT_NAME}/threadx target configured")
message(STATUS "ThreadX: tx_user.h     = ${RA8_THREADX_PORT_DIR}/inc/tx_user.h")
message(STATUS "ThreadX: low-level S   = ${RA8_THREADX_PROJECT_LOW_LEVEL}")

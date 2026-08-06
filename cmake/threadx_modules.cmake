# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/threadx_modules.cmake
#
# Adds the ThreadX Module Manager library for loading and running isolated
# third-party application modules on the Cortex-M85.
#
# Usage from a per-app CMakeLists.txt:
#
#     option(RA8_USE_THREADX_MODULES "Enable ThreadX Module Manager" OFF)
#     if(RA8_USE_THREADX_MODULES)
#         include(${RA8_REPO_ROOT}/cmake/threadx_modules.cmake)
#         target_link_libraries(<app>.elf PRIVATE threadx_modules)
#     endif()
#
# This is layered on top of the base ThreadX kernel. An app must also
# include cmake/threadx.cmake and link against `threadx`.
#
# What it does:
#
#   - Compiles the common module manager C sources from
#     libs/third_party/threadx/common_modules/module_manager/src/*.c
#   - Compiles the M85-specific port sources from
#     libs/third_party/threadx/ports_module/cortex_m85/gnu/module_manager/src/
#   - Exposes the module manager headers through PUBLIC includes.
#

if(TARGET threadx_modules)
  return()
endif()

if(NOT TARGET threadx)
  message(FATAL_ERROR "threadx_modules requires the 'threadx' target. "
                      "Include cmake/threadx.cmake first.")
endif()

if(NOT DEFINED RA8_REPO_ROOT)
  get_filename_component(RA8_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(RA8_THREADX_ROOT "${RA8_REPO_ROOT}/libs/third_party/threadx")
set(RA8_TXM_PORT_DIR "${RA8_THREADX_ROOT}/ports_module/cortex_m85/gnu")

if(NOT IS_DIRECTORY "${RA8_TXM_PORT_DIR}")
  message(FATAL_ERROR "ThreadX Module M85 port not found at ${RA8_TXM_PORT_DIR}. "
                      "Run the M85 port creation first (Issue #659).")
endif()

# ---------------------------------------------------------------------------
# Common Module Manager C sources (kernel-side dispatch, loading, lifecycle).
# ---------------------------------------------------------------------------
file(GLOB RA8_TXM_COMMON_SOURCES CONFIGURE_DEPENDS
  "${RA8_THREADX_ROOT}/common_modules/module_manager/src/*.c"
)

# ---------------------------------------------------------------------------
# M85 port-specific sources (MPU setup, stack build, context switch, etc.).
# ---------------------------------------------------------------------------
file(GLOB RA8_TXM_PORT_ASM CONFIGURE_DEPENDS
  "${RA8_TXM_PORT_DIR}/module_manager/src/*.S"
  "${RA8_TXM_PORT_DIR}/module_manager/src/*.s"
)
file(GLOB RA8_TXM_PORT_C CONFIGURE_DEPENDS
  "${RA8_TXM_PORT_DIR}/module_manager/src/*.c"
)

# Drop the upstream low-level init if present (the base threadx target
# already provides our project-tuned version).
list(FILTER RA8_TXM_PORT_ASM EXCLUDE REGEX ".*/tx_initialize_low_level\\.S$")

# ---------------------------------------------------------------------------
# Build the static library.
# ---------------------------------------------------------------------------
add_library(
  threadx_modules STATIC
  ${RA8_TXM_COMMON_SOURCES}
  ${RA8_TXM_PORT_ASM}
  ${RA8_TXM_PORT_C}
)

# Link against the base ThreadX kernel.
target_link_libraries(threadx_modules PUBLIC threadx)

# Module Manager headers: common + port-specific.
target_include_directories(
  threadx_modules SYSTEM PUBLIC
  "${RA8_THREADX_ROOT}/common_modules/inc"
  "${RA8_THREADX_ROOT}/common_modules/module_manager/inc"
  "${RA8_TXM_PORT_DIR}/inc"
  "${RA8_TXM_PORT_DIR}/module_manager/inc"
)

# The module manager needs ra8_core/ra8_hal headers for the same
# reasons as the base kernel (ra8_cgc.h, ra8_log.h).
target_include_directories(
  threadx_modules PRIVATE
  "${RA8_REPO_ROOT}/libs/ra8_core/inc"
  "${RA8_REPO_ROOT}/libs/ra8_hal/inc"
)

# Enable the TX_MODULE_MANAGER_16_MPU flag for the M85's 16-region MPU.
target_compile_definitions(threadx_modules PUBLIC TXM_MODULE_ENABLE)

# The module manager requires notify callbacks (trampolines). tx_user.h
# defines TX_DISABLE_NOTIFY_CALLBACKS which compiles out the trampoline
# bodies in the GLOB'd sources. Exclude them and use a wrapper that
# #undef's the macro before #include-ing the .c files.
set(_txm_trampoline_re "txm_module_manager_.*_notify_trampoline\\.c$")
get_target_property(_txm_all_srcs threadx_modules SOURCES)
foreach(_src ${_txm_all_srcs})
  if("${_src}" MATCHES "${_txm_trampoline_re}")
    set_source_files_properties("${_src}" PROPERTIES HEADER_FILE_ONLY TRUE)
  endif()
endforeach()
target_sources(threadx_modules PRIVATE
  "${RA8_REPO_ROOT}/port/threadx/src/module/txm_trampolines.c"
)
# Add the source dir so the wrapper can #include the upstream .c files.
target_include_directories(threadx_modules PRIVATE
  "${RA8_THREADX_ROOT}/common_modules/module_manager/src"
)

# Quiet upstream warnings (same treatment as base threadx target).
target_compile_options(threadx_modules PRIVATE $<$<COMPILE_LANGUAGE:C>:-w>)

# Match the base threadx target's C standard.
set_target_properties(threadx_modules PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)

message(STATUS "ThreadX Modules: threadx_modules target configured")
message(STATUS "ThreadX Modules: M85 port = ${RA8_TXM_PORT_DIR}")

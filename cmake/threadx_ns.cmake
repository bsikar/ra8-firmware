# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/threadx_ns.cmake
#
# Non-Secure variant of the ThreadX library (Cortex-M85), for apps that run
# the RTOS INSIDE the TrustZone Non-Secure image (see GitHub #96 -- Phase C of
# tz_nsc_cgc_usb). Identical sources to cmake/threadx.cmake, but compiled with
# RA8_THREADX_NON_SECURE so port/threadx/inc/tx_user.h selects TX_SINGLE_MODE_NON_SECURE
# instead of TX_SINGLE_MODE_SECURE. The consuming app's linker script routes
# this archive's sections into the NS regions.
#
# Usage from a per-app CMakeLists.txt:
#
#     include(${RA8_REPO_ROOT}/cmake/threadx_ns.cmake)
#     target_link_libraries(<app>.elf PRIVATE threadx_ns)
#
#

if(TARGET threadx_ns)
  return()
endif()

if(NOT DEFINED RA8_REPO_ROOT)
  get_filename_component(RA8_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(RA8_THREADX_ROOT "${RA8_REPO_ROOT}/libs/third_party/threadx")
set(RA8_THREADX_PORT_DIR "${RA8_REPO_ROOT}/port/threadx")
set(RA8_THREADX_M85_GNU "${RA8_THREADX_ROOT}/ports/cortex_m85/gnu")

if(NOT IS_DIRECTORY "${RA8_THREADX_M85_GNU}")
  message(FATAL_ERROR "ThreadX Cortex-M85 GNU port not found at ${RA8_THREADX_M85_GNU}")
endif()

file(GLOB RA8_THREADX_NS_COMMON_SOURCES CONFIGURE_DEPENDS "${RA8_THREADX_ROOT}/common/src/*.c")
file(GLOB RA8_THREADX_NS_PORT_ASM CONFIGURE_DEPENDS "${RA8_THREADX_M85_GNU}/src/*.S")
file(GLOB RA8_THREADX_NS_PORT_C CONFIGURE_DEPENDS "${RA8_THREADX_M85_GNU}/src/*.c")

# Drop the upstream low-level init -- replaced by the project-tuned version.
list(
  FILTER
  RA8_THREADX_NS_PORT_ASM
  EXCLUDE
  REGEX
  ".*/tx_initialize_low_level\\.S$"
)

set(RA8_THREADX_NS_PROJECT_LOW_LEVEL
    "${RA8_THREADX_PORT_DIR}/src/cortex_m85/tx_initialize_low_level.S"
)

add_library(
  threadx_ns STATIC
  ${RA8_THREADX_NS_COMMON_SOURCES}
  ${RA8_THREADX_NS_PORT_ASM}
  ${RA8_THREADX_NS_PORT_C}
  ${RA8_THREADX_NS_PROJECT_LOW_LEVEL}
  "${RA8_THREADX_PORT_DIR}/src/cortex_m85/tx_systick_ready.c"
)

target_include_directories(
  threadx_ns SYSTEM PUBLIC "${RA8_THREADX_ROOT}/common/inc" "${RA8_THREADX_M85_GNU}/inc"
)
target_include_directories(threadx_ns PUBLIC "${RA8_THREADX_PORT_DIR}/inc")

# RA8_THREADX_NON_SECURE flips tx_user.h to TX_SINGLE_MODE_NON_SECURE. PUBLIC so
# the consuming app's TUs (ns_main.c) see the same kernel-option view.
target_compile_definitions(threadx_ns PUBLIC TX_INCLUDE_USER_DEFINE_FILE RA8_THREADX_NON_SECURE)

target_link_options(
  threadx_ns INTERFACE -Wl,--undefined=_tx_timer_interrupt
  -Wl,--undefined=g_ra8_threadx_systick_ready
)

# Upstream sources trip a few warnings the firmware build elevates to errors.
target_compile_options(threadx_ns PRIVATE $<$<COMPILE_LANGUAGE:C>:-w>)

message(STATUS "ThreadX-NS: threadx_ns target configured (TX_SINGLE_MODE_NON_SECURE)")

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# CMakeLists.txt for tz_threadx_demo.
#
# Configures a two-project TrustZone build:
# 1. tz_threadx_demo.elf (Secure Boot + NSC Veneers)
# 2. tz_threadx_demo_ns.elf (Non-Secure ThreadX multitasking demo)
#
#

cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(tz_threadx_demo LANGUAGES C ASM)
endif()

# Dynamically resolve repository root directory
set(RA8_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${RA8_REPO_ROOT}/cmake/ra8_add_app.cmake" AND NOT "${RA8_REPO_ROOT}" STREQUAL "/")
  get_filename_component(RA8_REPO_ROOT "${RA8_REPO_ROOT}" DIRECTORY)
endwhile()

# Include the shared per-app build macro
include("${RA8_REPO_ROOT}/cmake/ra8_add_app.cmake")

# ---- Secure Target --------------------------------------------------------
ra8_add_app(
  NAME tz_threadx_demo
  STACK_BYTES 2200
  DESCRIPTION "TrustZone ThreadX Demo (Secure side)" AUX_SRCS src/ns_main.c
  LIBS ra8_tz_secure_boot MRAM_LENGTH 512K
)

include(${RA8_REPO_ROOT}/libs/ra8_board_ek_ra8d2/ld/ns_memory_map.cmake)
# The Secure boot copies/jumps to the NS image, so it needs the NS addresses as
# constants. They come from the same map the NS linker script is generated from
# rather than being re-typed in trustzone_init.c (#759 item 2).
ra8_ns_memory_map_defines(tz_threadx_demo.elf)

target_compile_definitions(tz_threadx_demo.elf PRIVATE RA8_TRUSTZONE_ENABLE RA8_APP_UART_LOG)
target_compile_options(tz_threadx_demo.elf PRIVATE -mcmse -fshort-enums -ffreestanding)

# ---- Post-build: verify the NSC SG-veneer layout (T5-08) ------------------
# tz_threadx_demo_ns.elf is bound to the Secure-Gateway stubs through the CMSE import
# library emitted by ra8_add_ns_image(), so the byte offset of each veneer
# inside .gnu.sgstubs IS the S/NS ABI: a reorder redirects every NS->Secure
# call with no diagnostic. This app's linker_script.ld already claims the
# offsets are "verified AFTER the link ... by scripts/checks/check_sg_offsets.py,
# run as a POST_BUILD step in this app's CMakeLists.txt". The checker requires
# one veneer per 8-byte slot across the whole region and self-skips a
# non-TrustZone link.
# --nm pins the SAME cross nm as the link (CMAKE_NM from the toolchain file).
add_custom_command(
  TARGET tz_threadx_demo.elf
  POST_BUILD
  COMMAND ${RA8_REPO_ROOT}/scripts/checks/check_sg_offsets.py $<TARGET_FILE:tz_threadx_demo.elf>
          --nm ${CMAKE_NM}
  VERBATIM
  COMMENT "check_sg_offsets: verifying NSC SG-veneer slot offsets (T5-08)"
)

# ---- Non-Secure Target ----------------------------------------------------
# ra8_add_ns_image() owns the CMSE implib handshake at both ends, the NS
# warning profile, the build ordering, and the merged-hex staple (#759 item 1).
include(${RA8_REPO_ROOT}/cmake/threadx_ns.cmake)

ra8_add_ns_image(
  SECURE_TARGET
  tz_threadx_demo.elf
  NAME
  tz_threadx_demo_ns
  STACK_BYTES
  2200
  IMPLIB
  ${CMAKE_CURRENT_BINARY_DIR}/tz_threadx_demo_cmse_import.o
  MERGED_HEX
  tz_threadx_demo.hex
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ns_main.c
  DEFINES
  RA8_TRUSTZONE_ENABLE
  RA8_PERIPH_NS_ALIAS
  INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/inc
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${RA8_REPO_ROOT}/libs/ra8_core/inc
  ${RA8_REPO_ROOT}/libs/ra8_hal/inc
  ${RA8_REPO_ROOT}/libs/ra8_nsc/inc
  ${RA8_REPO_ROOT}/libs/ra8_board_ek_ra8d2/inc
  LINK_LIBS
  threadx_ns
)

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
  DESCRIPTION "TrustZone ThreadX Demo (Secure side)"
  LIBS ra8_tz_secure_boot
)

set(_ra8_tz_implib ${CMAKE_CURRENT_BINARY_DIR}/tz_threadx_demo_cmse_import.o)
target_compile_definitions(tz_threadx_demo.elf PRIVATE RA8_TRUSTZONE_ENABLE RA8_APP_UART_LOG)
target_compile_options(tz_threadx_demo.elf PRIVATE -mcmse -fshort-enums -ffreestanding)

target_link_options(
  tz_threadx_demo.elf
  PRIVATE
  -nostartfiles
  -T${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld
  -Wl,--Map=tz_threadx_demo.map
  -Wl,--cmse-implib
  -Wl,--out-implib=${_ra8_tz_implib}
)

set_target_properties(
  tz_threadx_demo.elf PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld"
)

# ---- Non-Secure Target ----------------------------------------------------
include(${RA8_REPO_ROOT}/cmake/threadx_ns.cmake)

add_executable(tz_threadx_demo_ns.elf ${CMAKE_CURRENT_SOURCE_DIR}/ns_main.c)

target_compile_definitions(tz_threadx_demo_ns.elf PRIVATE RA8_TRUSTZONE_ENABLE RA8_PERIPH_NS_ALIAS)

target_compile_options(tz_threadx_demo_ns.elf PRIVATE -fshort-enums -ffreestanding)

# Raw add_executable targets skip the ra8_add_app warning profile, so without
# this the NS image compiles WITHOUT -Wall/-Wextra/-Werror (the T2-05
# warnings-scope hole). threadx_ns keeps its own permissive vendor flags.
ra8_target_enable_project_warnings(tz_threadx_demo_ns.elf STACK_USAGE_BYTES 2200)

target_include_directories(
  tz_threadx_demo_ns.elf
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${RA8_REPO_ROOT}/libs/ra8_core/inc
          ${RA8_REPO_ROOT}/libs/ra8_hal/inc
          ${RA8_REPO_ROOT}/libs/ra8_nsc/inc
          ${RA8_REPO_ROOT}/libs/ra8_board_ek_ra8d2/inc
)

target_link_libraries(tz_threadx_demo_ns.elf PRIVATE threadx_ns)

target_link_options(
  tz_threadx_demo_ns.elf
  PRIVATE
  -nostartfiles
  -T${CMAKE_CURRENT_SOURCE_DIR}/ns_image.ld
  -Wl,--Map=tz_threadx_demo_ns.map
  ${_ra8_tz_implib}
)

set_target_properties(
  tz_threadx_demo_ns.elf PROPERTIES LINK_DEPENDS
                                    "${CMAKE_CURRENT_SOURCE_DIR}/ns_image.ld;${_ra8_tz_implib}"
)

# Ensure the Secure ELF is built before the NS ELF.
add_dependencies(tz_threadx_demo_ns.elf tz_threadx_demo.elf)

# Combine: Secure image + NS image -> one merged flashable hex
add_custom_command(
  TARGET tz_threadx_demo_ns.elf
  POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} --remove-section=.option_setting* -O ihex
          $<TARGET_FILE:tz_threadx_demo.elf> tz_threadx_demo_secure.hex
  COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:tz_threadx_demo_ns.elf> tz_threadx_demo_ns.hex
  COMMAND ${RA8_REPO_ROOT}/scripts/gen/merge_ihex.py tz_threadx_demo_secure.hex
          tz_threadx_demo_ns.hex tz_threadx_demo.hex
  VERBATIM
  COMMENT "Merging Secure + Non-Secure images into tz_threadx_demo.hex"
)

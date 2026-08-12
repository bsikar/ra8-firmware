# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# CMakeLists.txt for the main e-reader application (src/app).
#
# Configures a two-project TrustZone build:
# 1. ra8d2-ereader.elf (Secure Boot + Key Vault + NSC Veneers)
#    Note: This is named to match the app name so standard emulator
#          and flashing tools boot the Secure entry.
# 2. ra8d2-ereader_ns.elf (Non-Secure UI + ThreadX + Graphics)
#
#

cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(ra8d2-ereader LANGUAGES C CXX ASM)
endif()

# Dynamically resolve repository root directory
set(RA8_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${RA8_REPO_ROOT}/cmake/ra8_add_app.cmake" AND NOT "${RA8_REPO_ROOT}" STREQUAL "/")
  get_filename_component(RA8_REPO_ROOT "${RA8_REPO_ROOT}" DIRECTORY)
endwhile()

# Include the shared per-app build macro
include("${RA8_REPO_ROOT}/cmake/ra8_add_app.cmake")

# ---- Secure Target --------------------------------------------------------
# Use the repository's standard macro to build the Secure side.
# This automatically handles include paths for ra8_core, ra8_hal, ra8_net_pal, ra8_usb_pal,
# ra8_board_ek_ra8d2, and compiles the Ring 5 src/secure_app/ sources.
ra8_add_app(
  NAME ra8d2-ereader
  STACK_BYTES 2200
  DESCRIPTION "Main e-reader Secure application (Bootloader + Vault)"
  LIBS ra8_tz_secure_boot
)

# Set TrustZone secure-side compiler options and output CMSE import library.
set(_ra8_tz_implib ${CMAKE_CURRENT_BINARY_DIR}/ra8d2_cmse_import.o)

# ---- OSPI XIP (execute-in-place) variant ----------------------------------
# RA8_EREADER_NS_XIP=ON links the Non-Secure image to run execute-in-place from
# OSPI flash (NS alias 0x9000_0000) instead of the default copy-to-SRAM run
# model: the Secure boot arms XIP + BLXNS instead of copying MRAM->SRAM, and
# ns_reset_handler copies only .data into SRAM. OFF by default so the proven
# SRAM-run e-reader stays the default build.
option(RA8_EREADER_NS_XIP "Run the Non-Secure e-reader XIP from OSPI flash" OFF)
if(RA8_EREADER_NS_XIP)
  set(_ra8_ns_linker "${CMAKE_CURRENT_SOURCE_DIR}/ns_image_xip.ld")
else()
  set(_ra8_ns_linker "${CMAKE_CURRENT_SOURCE_DIR}/ns_image.ld")
endif()

# RA8_APP_UART_LOG mirrors ra8_log onto the SCI8 console (in ra8_nsc_periph_init) so
# the e-reader's logs appear on the UART -- ra8_emulator's on-screen console panel
# and a real serial terminal -- not only on the ITM/SWO trace.
target_compile_definitions(
  ra8d2-ereader.elf PRIVATE RA8_TRUSTZONE_ENABLE RA8_APP_UART_LOG
                            $<$<BOOL:${RA8_EREADER_NS_XIP}>:RA8_EREADER_NS_XIP>
)
target_compile_options(ra8d2-ereader.elf PRIVATE -mcmse -fshort-enums -ffreestanding)
target_include_directories(ra8d2-ereader.elf PRIVATE ${RA8_REPO_ROOT}/src/secure_app)

target_link_options(
  ra8d2-ereader.elf
  PRIVATE
  -nostartfiles
  -T${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld
  -Wl,--Map=ra8d2-ereader.map
  -Wl,--cmse-implib
  -Wl,--out-implib=${_ra8_tz_implib}
)

set_target_properties(
  ra8d2-ereader.elf PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld"
)

# ---- Non-Secure Target ----------------------------------------------------
# Configures the Non-Secure ThreadX OS and the e-reader UI.
include(${RA8_REPO_ROOT}/cmake/threadx_ns.cmake)

# The NS world runs the ThreadX watchdog supervisor; pull in its sources plus
# ra8_wdt.c (the supervisor's default refresh hook references
# ra8_wdt_refresh_deferred, so the symbol must resolve -- it is dead code once
# ns_main routes the hook through the ra8_nsc_wdt_refresh veneer).
file(GLOB _ra8_ns_wdt_sup CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_wdt_supervisor/src/*.c)

add_executable(
  ra8d2-ereader_ns.elf ${CMAKE_CURRENT_SOURCE_DIR}/ns_main.c ${_ra8_ns_wdt_sup}
                       ${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_wdt.c
)

target_compile_definitions(
  ra8d2-ereader_ns.elf PRIVATE RA8_TRUSTZONE_ENABLE RA8_PERIPH_NS_ALIAS RA8_USB_POLLED_ONLY
                               $<$<BOOL:${RA8_EREADER_NS_XIP}>:RA8_EREADER_NS_XIP>
)

target_compile_options(ra8d2-ereader_ns.elf PRIVATE -fshort-enums -ffreestanding)

# The NS image is a raw add_executable, so unlike the Secure side (which gets
# the profile via ra8_add_app) it would otherwise compile WITHOUT
# -Wall/-Wextra/-Werror -- the T2-05 warnings-scope hole: a discarded
# [[nodiscard]] ra8_err_t in ns_main.c could never fail the build. Apply the
# same first-party warning profile; threadx_ns is a separate target and keeps
# its own permissive vendor flags.
ra8_target_enable_project_warnings(ra8d2-ereader_ns.elf STACK_USAGE_BYTES 2200)

target_include_directories(
  ra8d2-ereader_ns.elf
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${RA8_REPO_ROOT}/libs/ra8_core/inc
          ${RA8_REPO_ROOT}/libs/ra8_hal/inc
          ${RA8_REPO_ROOT}/libs/ra8_nsc/inc
          ${RA8_REPO_ROOT}/libs/ra8_board_ek_ra8d2/inc
          ${RA8_REPO_ROOT}/libs/ra8_wdt_supervisor/inc
)

target_link_libraries(ra8d2-ereader_ns.elf PRIVATE threadx_ns)

target_link_options(
  ra8d2-ereader_ns.elf
  PRIVATE
  -nostartfiles
  -T${_ra8_ns_linker}
  -Wl,--Map=ra8d2-ereader_ns.map
  ${_ra8_tz_implib}
)

set_target_properties(
  ra8d2-ereader_ns.elf PROPERTIES LINK_DEPENDS "${_ra8_ns_linker};${_ra8_tz_implib}"
)

# Ensure the Secure ELF (and its import library) is built before the NS ELF.
add_dependencies(ra8d2-ereader_ns.elf ra8d2-ereader.elf)

# Combine: Secure image + NS image -> one merged flashable hex
add_custom_command(
  TARGET ra8d2-ereader_ns.elf
  POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} --remove-section=.option_setting* -O ihex
          $<TARGET_FILE:ra8d2-ereader.elf> ra8d2-ereader_secure.hex
  COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:ra8d2-ereader_ns.elf> ra8d2-ereader_ns.hex
  COMMAND ${RA8_REPO_ROOT}/scripts/gen/merge_ihex.py ra8d2-ereader_secure.hex ra8d2-ereader_ns.hex
          ra8d2-ereader.hex
  VERBATIM
  COMMENT "Merging Secure + Non-Secure images into ra8d2-ereader.hex"
)

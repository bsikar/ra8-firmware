# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# CMakeLists.txt for the main e-reader application (apps/board/stand_alone/ereader).
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
# ra8_board_ek_ra8d2, and compiles the Ring 5 libs/ra8_secure_app/ sources.
ra8_add_app(
  NAME ra8d2-ereader
  STACK_BYTES 2200 MRAM_LENGTH 512K
  DESCRIPTION "Main e-reader Secure application (Bootloader + Vault)"
  LIBS ra8_tz_secure_boot AUX_SRCS src/ns_main.c
)

# ---- OSPI XIP (execute-in-place) variant ----------------------------------
# RA8_EREADER_NS_XIP=ON links the Non-Secure image to run execute-in-place from
# OSPI flash (NS alias 0x9000_0000) instead of the default copy-to-SRAM run
# model: the Secure boot arms XIP + BLXNS instead of copying MRAM->SRAM, and
# ns_reset_handler copies only .data into SRAM. OFF by default so the proven
# SRAM-run e-reader stays the default build.
option(RA8_EREADER_NS_XIP "Run the Non-Secure e-reader XIP from OSPI flash" OFF)
# ra8_add_ns_image() generates the script from the board template; the option
# selects the read-only home rather than a second copy of the script.
if(RA8_EREADER_NS_XIP)
  set(_ra8_ns_xip XIP)
else()
  set(_ra8_ns_xip)
endif()

# RA8_APP_UART_LOG mirrors ra8_log onto the SCI8 console (in ra8_nsc_periph_init) so
# the e-reader's logs appear on the UART -- ra8_emulator's on-screen console panel
# and a real serial terminal -- not only on the ITM/SWO trace.
include(${RA8_REPO_ROOT}/libs/ra8_board_ek_ra8d2/ld/ns_memory_map.cmake)
# The Secure boot copies/jumps to the NS image, so it needs the NS addresses as
# constants. They come from the same map the NS linker script is generated from
# rather than being re-typed in trustzone_init.c (#759 item 2).
ra8_ns_memory_map_defines(ra8d2-ereader.elf)

target_compile_definitions(
  ra8d2-ereader.elf PRIVATE RA8_TRUSTZONE_ENABLE RA8_APP_UART_LOG
                            $<$<BOOL:${RA8_EREADER_NS_XIP}>:RA8_EREADER_NS_XIP>
)
target_compile_options(ra8d2-ereader.elf PRIVATE -mcmse -fshort-enums -ffreestanding)

# ---- Post-build: verify the NSC SG-veneer layout (T5-08) ------------------
# ra8d2-ereader_ns.elf is bound to the Secure-Gateway stubs through the CMSE import
# library emitted by ra8_add_ns_image(), so the byte offset of each veneer
# inside .gnu.sgstubs IS the S/NS ABI: a reorder redirects every NS->Secure
# call with no diagnostic. The board map derives the veneer bounds from the
# finished .gnu.sgstubs section, and this POST_BUILD step is what proves the
# slot layout the NS side is bound to. The checker requires one veneer per
# 8-byte slot across the whole region and self-skips a non-TrustZone link.
# --nm pins the SAME cross nm as the link (CMAKE_NM from the toolchain file).
add_custom_command(
  TARGET ra8d2-ereader.elf
  POST_BUILD
  COMMAND ${RA8_REPO_ROOT}/scripts/checks/check_sg_offsets.py $<TARGET_FILE:ra8d2-ereader.elf> --nm
          ${CMAKE_NM}
  VERBATIM
  COMMENT "check_sg_offsets: verifying NSC SG-veneer slot offsets (T5-08)"
)

# ---- Non-Secure Target ----------------------------------------------------
# Configures the Non-Secure ThreadX OS and the e-reader UI. ra8_add_ns_image()
# owns the CMSE implib handshake at both ends, the NS warning profile, the
# build ordering, and the merged-hex staple (#759 item 1).
include(${RA8_REPO_ROOT}/cmake/threadx_ns.cmake)

# The NS world runs the ThreadX watchdog supervisor. The supervisor is Zig
# now (libs/ra8_wdt_supervisor/src holds no .c), so its objects arrive as a
# static archive rather than as sources compiled into the image. It is named
# in LINK_LIBS ahead of threadx_ns because the supervisor calls into ThreadX:
# appended after it, _tx_time_get and the six _txe_* entry points go
# unresolved. ra8_wdt.c still comes in as a source: the supervisor's default
# refresh hook references ra8_wdt_refresh_deferred, so the symbol must resolve
# -- it is dead code once ns_main routes the hook through the
# ra8_nsc_wdt_refresh veneer.
ra8_zig_archive_for_cpu(
  _ra8_ns_wdt_sup
  LIB
  ra8_wdt_supervisor
  CPU
  cortex_m85
  FLOAT
  hard
)

ra8_add_ns_image(
  SECURE_TARGET
  ra8d2-ereader.elf
  NAME
  ra8d2-ereader_ns
  ${_ra8_ns_xip}
  STACK_BYTES
  2200
  IMPLIB
  ${CMAKE_CURRENT_BINARY_DIR}/ra8d2_cmse_import.o
  MERGED_HEX
  ra8d2-ereader.hex
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ns_main.c
  ${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_wdt.c
  DEFINES
  RA8_TRUSTZONE_ENABLE
  RA8_PERIPH_NS_ALIAS
  RA8_USB_POLLED_ONLY
  $<$<BOOL:${RA8_EREADER_NS_XIP}>:RA8_EREADER_NS_XIP>
  INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${RA8_REPO_ROOT}/libs/ra8_core/inc
  ${RA8_REPO_ROOT}/libs/ra8_hal/inc
  ${RA8_REPO_ROOT}/libs/ra8_nsc/inc
  ${RA8_REPO_ROOT}/libs/ra8_board_ek_ra8d2/inc
  ${RA8_REPO_ROOT}/libs/ra8_wdt_supervisor/inc
  LINK_LIBS
  ${_ra8_ns_wdt_sup}
  threadx_ns
)

# The archive is produced by a rule in this directory and named by full path on
# the link line, so ninja already orders the two. State the edge anyway: the
# image is also reachable as a bare `ninja ra8d2-ereader_ns.elf`.
add_custom_target(ra8d2-ereader_ns_zig_wdt_supervisor DEPENDS ${_ra8_ns_wdt_sup})
add_dependencies(ra8d2-ereader_ns.elf ra8d2-ereader_ns_zig_wdt_supervisor)

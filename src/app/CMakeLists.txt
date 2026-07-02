#
# CMakeLists.txt for the main e-reader application (src/app).
#
# Configures a two-project TrustZone build:
# 1. ra8d2-ereader.elf (Secure Boot + Key Vault + NSC Veneers)
#    Note: This is named to match the app name so standard simulator
#          and flashing tools boot the Secure entry.
# 2. ra8d2-ereader_ns.elf (Non-Secure UI + ThreadX + Graphics)
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra_has_parent PARENT_DIRECTORY)
if(NOT _ra_has_parent)
    project(ra8d2-ereader LANGUAGES C ASM)
endif()

# Dynamically resolve repository root directory
set(RA_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${RA_REPO_ROOT}/cmake/ra_add_app.cmake" AND NOT "${RA_REPO_ROOT}" STREQUAL "/")
    get_filename_component(RA_REPO_ROOT "${RA_REPO_ROOT}" DIRECTORY)
endwhile()

# Include the shared per-app build macro
include("${RA_REPO_ROOT}/cmake/ra_add_app.cmake")

# ---- Secure Target --------------------------------------------------------
# Use the repository's standard macro to build the Secure side.
# This automatically handles include paths for ra_core, ra_hal, ra_net_pal, ra_usb_pal,
# ra_board_ek_ra8d2, and compiles the Ring 5 src/secure_app/ sources.
ra_add_app(NAME ra8d2-ereader STACK_BYTES 2200
           DESCRIPTION "Main e-reader Secure application (Bootloader + Vault)"
           LIBS ra_tz_secure_boot)

# Set TrustZone secure-side compiler options and output CMSE import library.
set(_ra_tz_implib ${CMAKE_CURRENT_BINARY_DIR}/ra8d2_cmse_import.o)

# ---- OSPI XIP (execute-in-place) variant ----------------------------------
# RA_EREADER_NS_XIP=ON links the Non-Secure image to run execute-in-place from
# OSPI flash (NS alias 0x9000_0000) instead of the default copy-to-SRAM run
# model: the Secure boot arms XIP + BLXNS instead of copying MRAM->SRAM, and
# ns_reset_handler copies only .data into SRAM. OFF by default so the proven
# SRAM-run e-reader stays the default build.
option(RA_EREADER_NS_XIP "Run the Non-Secure e-reader XIP from OSPI flash" OFF)
if(RA_EREADER_NS_XIP)
    set(_ra_ns_linker "${CMAKE_CURRENT_SOURCE_DIR}/ns_image_xip.ld")
else()
    set(_ra_ns_linker "${CMAKE_CURRENT_SOURCE_DIR}/ns_image.ld")
endif()

# RA_APP_UART_LOG mirrors ra_log onto the SCI8 console (in ra_nsc_periph_init) so
# the e-reader's logs appear on the UART -- board_sim's on-screen console panel
# and a real serial terminal -- not only on the ITM/SWO trace.
target_compile_definitions(ra8d2-ereader.elf PRIVATE RA_TRUSTZONE_ENABLE RA_APP_UART_LOG
    $<$<BOOL:${RA_EREADER_NS_XIP}>:RA_EREADER_NS_XIP>)
target_compile_options(ra8d2-ereader.elf PRIVATE -mcmse -fshort-enums -ffreestanding)
target_include_directories(ra8d2-ereader.elf PRIVATE ${RA_REPO_ROOT}/src/secure_app)

target_link_options(ra8d2-ereader.elf PRIVATE
    -nostartfiles
    -T${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld
    -Wl,--Map=ra8d2-ereader.map
    -Wl,--cmse-implib
    -Wl,--out-implib=${_ra_tz_implib}
)

set_target_properties(ra8d2-ereader.elf PROPERTIES
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/linker_script.ld"
)

# ---- Non-Secure Target ----------------------------------------------------
# Configures the Non-Secure ThreadX OS and the e-reader UI.
include(${RA_REPO_ROOT}/cmake/threadx_ns.cmake)

# The NS world runs the ThreadX watchdog supervisor; pull in its sources plus
# ra_wdt.c (the supervisor's default refresh hook references
# ra_wdt_refresh_deferred, so the symbol must resolve -- it is dead code once
# ns_main routes the hook through the ra_nsc_wdt_refresh veneer).
file(GLOB _ra_ns_wdt_sup CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_wdt_supervisor/src/*.c)

add_executable(ra8d2-ereader_ns.elf
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_main.c
    ${_ra_ns_wdt_sup}
    ${RA_REPO_ROOT}/libs/ra_hal/src/ra_wdt.c
)

target_compile_definitions(ra8d2-ereader_ns.elf PRIVATE
    RA_TRUSTZONE_ENABLE
    RA_PERIPH_NS_ALIAS
    RA_USB_POLLED_ONLY
    $<$<BOOL:${RA_EREADER_NS_XIP}>:RA_EREADER_NS_XIP>
)

target_compile_options(ra8d2-ereader_ns.elf PRIVATE
    -fshort-enums
    -ffreestanding
)

target_include_directories(ra8d2-ereader_ns.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RA_REPO_ROOT}/libs/ra_core/inc
    ${RA_REPO_ROOT}/libs/ra_hal/inc
    ${RA_REPO_ROOT}/libs/ra_nsc/inc
    ${RA_REPO_ROOT}/libs/ra_board_ek_ra8d2/inc
    ${RA_REPO_ROOT}/libs/ra_wdt_supervisor/inc
)

target_link_libraries(ra8d2-ereader_ns.elf PRIVATE
    threadx_ns
)

target_link_options(ra8d2-ereader_ns.elf PRIVATE
    -nostartfiles
    -T${_ra_ns_linker}
    -Wl,--Map=ra8d2-ereader_ns.map
    ${_ra_tz_implib}
)

set_target_properties(ra8d2-ereader_ns.elf PROPERTIES
    LINK_DEPENDS "${_ra_ns_linker};${_ra_tz_implib}"
)

# Ensure the Secure ELF (and its import library) is built before the NS ELF.
add_dependencies(ra8d2-ereader_ns.elf ra8d2-ereader.elf)

# Combine: Secure image + NS image -> one merged flashable hex
add_custom_command(TARGET ra8d2-ereader_ns.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} --remove-section=.option_setting* -O ihex
            $<TARGET_FILE:ra8d2-ereader.elf> ra8d2-ereader_secure.hex
    COMMAND ${CMAKE_OBJCOPY} -O ihex
            $<TARGET_FILE:ra8d2-ereader_ns.elf> ra8d2-ereader_ns.hex
    COMMAND ${RA_REPO_ROOT}/scripts/utils/merge_ihex.py
            ra8d2-ereader_secure.hex ra8d2-ereader_ns.hex ra8d2-ereader.hex
    VERBATIM
    COMMENT "Merging Secure + Non-Secure images into ra8d2-ereader.hex"
)

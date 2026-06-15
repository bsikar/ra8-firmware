#
# Per-app CMake stub for `tz_nsc_cgc_usb`. All build logic lives in the shared
# cmake/ra_add_app.cmake recipe; this file only names the app + its deps.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra_add_app.cmake")

# This app's whole point is the NSC veneer wall (NS main calls Secure CGC
# via cmse_nonsecure_entry trampolines); without TZ the veneers compile as
# regular functions and the test is meaningless. Default TZ ON in standalone.
get_directory_property(_ra_p PARENT_DIRECTORY)
if(NOT _ra_p)
    option(RA_TRUSTZONE_ENABLE "Enable Cortex-M85 TrustZone (-mcmse + SAU)" ON)
endif()

# Phase A+B (#55) is a minimal S->NS BLXNS scaffold: linker partitions
# MRAM/SRAM into Secure + Non-Secure halves, secure SystemInit BLXNS-es
# into the NS image's reset handler, NS image runs a counter loop the
# bench can probe. ThreadX + USBX (USES threadx usbx) come back in
# Phase C once the linker can place their vendored object files inside
# NS_MRAM and the USB-FS peripheral is NS-attributed via an extra SAU
# region.
ra_add_app(NAME tz_nsc_cgc_usb STACK_BYTES 2200
           DESCRIPTION "TrustZone NSC-veneer + CGC USB-FS demo (Phase C: NSC CGC veneers from NS)"
           LIBS ra_tz_secure_boot
           NSC_SRCS ra_nsc_cgc.c)

# ns_main.c is the Non-Secure image (vector table + reset handler whose
# run-time VMA is the SRAM NS alias 0x32100000). It's not in ra_add_app's
# per-app source list because most apps don't have a NS half; add it here.
target_sources(tz_nsc_cgc_usb.elf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ns_main.c)

# Post-build: ns_main.c calls the NSC CGC veneers by address
# (g_ra_ls_sgstubs_start + k_sg_off_*), so verify ld has not reordered the
# .gnu.sgstubs slots. The check reads the final symbol table (where the SG-stub
# addresses are correct, unlike a linker ASSERT which sees __acle_se_*).
find_program(RA_PYTHON3 NAMES python3 python)
if(RA_PYTHON3)
    add_custom_command(TARGET tz_nsc_cgc_usb.elf POST_BUILD
        COMMAND ${RA_PYTHON3} ${_d}/scripts/utils/check_sg_offsets.py
                $<TARGET_FILE:tz_nsc_cgc_usb.elf>
        VERBATIM
        COMMENT "Verifying NSC SG-veneer slot offsets")
endif()

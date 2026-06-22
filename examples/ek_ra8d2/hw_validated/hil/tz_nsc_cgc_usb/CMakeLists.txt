#
# Per-app CMake for `tz_nsc_cgc_usb` -- a TWO-PROJECT TrustZone build (#96).
#
# The Secure ELF (ra_add_app) holds the secure boot + the NSC CGC veneers and
# emits a CMSE import library (--out-implib). A SEPARATE Non-Secure executable
# (tz_nsc_cgc_usb_ns.elf) links ns_main + ThreadX (+ USBX/ra_usb later) against
# that import library and its own private libc/libgcc, placed at the NS
# addresses. The two images are merged into one flashable hex. NS->Secure calls
# go only through the import library's SG veneers.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level
# listfile, which CMake requires to contain a literal, direct project()
# call. The aggregate repo-root build instead pulls this app in via
# add_subdirectory() (project() is already called at the root), so guard on
# PARENT_DIRECTORY to avoid a redundant nested sub-project. ra_add_app()
# below re-runs project() in the standalone case to apply the shared VERSION
# and the app description.
get_directory_property(_ra_has_parent PARENT_DIRECTORY)
if(NOT _ra_has_parent)
    project(tz_nsc_cgc_usb LANGUAGES C ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra_add_app.cmake")

get_directory_property(_ra_p PARENT_DIRECTORY)
if(NOT _ra_p)
    option(RA_TRUSTZONE_ENABLE "Enable Cortex-M85 TrustZone (-mcmse + SAU)" ON)
endif()

# ---- Secure ELF -----------------------------------------------------------
# Secure boot + NSC CGC veneers. ns_main / ThreadX are NOT here -- they are the
# Non-Secure executable below.
ra_add_app(NAME tz_nsc_cgc_usb STACK_BYTES 2200
           DESCRIPTION "TrustZone two-project: Secure side (CGC veneers + NS boot)"
           LIBS ra_tz_secure_boot
           NSC_SRCS ra_nsc_cgc.c)

# Emit the CMSE import library so the NS link can bind the veneer names to the
# Secure-Gateway stubs.
set(_ra_tz_implib ${CMAKE_CURRENT_BINARY_DIR}/tz_nsc_cgc_usb_cmse_import.o)
target_link_options(tz_nsc_cgc_usb.elf PRIVATE
    -Wl,--cmse-implib
    -Wl,--out-implib=${_ra_tz_implib})

# ---- Non-Secure executable ------------------------------------------------
include(${_d}/cmake/threadx_ns.cmake)

add_executable(tz_nsc_cgc_usb_ns.elf
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_usb.c)

# Match the toolchain ABI of the Secure side but build for the NS world: no
# -mcmse (NS emits no veneers), no startup files (the Secure boot copies .data
# and ns_reset_handler zeros .bss), and the NS-image linker script.
#   RA_PERIPH_NS_ALIAS -- ra_usb/ra_mstp reach the controllers through the IDAU
#                         bit[28]=1 Non-secure alias (0x5025_0000 / 0x5020_3000).
#   RA_USB_POLLED_ONLY -- ux_dcd_ra_usb skips the Secure-attributed USB NVIC line;
#                         the worker drives the controller via ra_usb_dispatch.
target_compile_definitions(tz_nsc_cgc_usb_ns.elf PRIVATE
    RA_TRUSTZONE_ENABLE RA_PERIPH_NS_ALIAS RA_USB_POLLED_ONLY)
target_compile_options(tz_nsc_cgc_usb_ns.elf PRIVATE -fshort-enums -ffreestanding)
target_include_directories(tz_nsc_cgc_usb_ns.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RA_REPO_ROOT}/libs/ra_core/inc
    ${RA_REPO_ROOT}/libs/ra_hal/inc
    ${RA_REPO_ROOT}/libs/ra_nsc/inc)
target_link_libraries(tz_nsc_cgc_usb_ns.elf PRIVATE threadx_ns)
target_link_options(tz_nsc_cgc_usb_ns.elf PRIVATE
    -nostartfiles
    -T${CMAKE_CURRENT_SOURCE_DIR}/ns_image.ld
    -Wl,--Map=tz_nsc_cgc_usb_ns.map
    ${_ra_tz_implib})
set_target_properties(tz_nsc_cgc_usb_ns.elf PROPERTIES
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ns_image.ld;${_ra_tz_implib}")
# The import library is produced by the Secure link, so build that first.
add_dependencies(tz_nsc_cgc_usb_ns.elf tz_nsc_cgc_usb.elf)

# ---- USBX + ra_usb, compiled NS-private into the NS ELF (#96 M2) -----------
# USBX core + CDC-ACM, the ra_usb<->USBX DCD bridge, the ra_usb register driver
# and the HAL modules ra_usb pulls in (ra_mstp/ra_log/ra_time/ra_usbhs). These
# are compiled FRESH into the NS image so it has its own copies -- the Secure
# side uses ra_log/ra_mstp/ra_time too, and a shared single-ELF copy would
# fault INVTRAN across the S/NS boundary.
set(_ra_usbx_root ${RA_REPO_ROOT}/libs/third_party/usbx)
file(GLOB _ns_usbx_core CONFIGURE_DEPENDS "${_ra_usbx_root}/common/core/src/*.c")
list(FILTER _ns_usbx_core EXCLUDE REGEX ".*/ux_(dcd_sim_slave|hcd_sim_host)_.*\\.c$")
file(GLOB _ns_usbx_cdc CONFIGURE_DEPENDS
     "${_ra_usbx_root}/common/usbx_device_classes/src/ux_device_class_cdc_acm_*.c")

# ra_time.c is intentionally NOT linked: its ra_time_init reprograms the SysTick
# ThreadX owns, and its delay path waits on a tick the ThreadX SysTick handler
# never bumps. ns_usb.c provides a ThreadX-backed ra_delay_ms instead (the only
# ra_time symbol ra_usb references).
set(_ns_usb_srcs
    ${_ns_usbx_core}
    ${_ns_usbx_cdc}
    ${RA_REPO_ROOT}/port/usbx/ux_dcd_ra_usb.c
    ${RA_REPO_ROOT}/libs/ra_hal/src/ra_usb.c
    ${RA_REPO_ROOT}/libs/ra_hal/src/ra_mstp.c
    ${RA_REPO_ROOT}/libs/ra_core/src/ra_log.c)
target_sources(tz_nsc_cgc_usb_ns.elf PRIVATE ${_ns_usb_srcs})
# Vendored USBX + ra_usb predate the first-party -Werror set.
set_source_files_properties(${_ns_usb_srcs} PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(tz_nsc_cgc_usb_ns.elf PRIVATE
    ${_ra_usbx_root}/common/core/inc
    ${_ra_usbx_root}/common/usbx_device_classes/inc
    ${_ra_usbx_root}/ports/cortex_m33/gnu/inc
    ${RA_REPO_ROOT}/port/usbx
    ${RA_REPO_ROOT}/libs/ra_usb_pal/inc
    ${RA_REPO_ROOT}/libs/ra_board_ek_ra8d2/inc)

# ---- Combine: Secure image + NS image -> one flashable hex ----------------
# objcopy each ELF to ihex (stripping the Secure side's option-setting records,
# which the J-Link/MRAM flasher cannot write), then merge -- the Secure image
# lives at 0x0200_0000+ and the NS image's LMA at 0x0208_0000, so they do not
# overlap. The merged hex overwrites the app hex so the existing flash flow
# (hil_run_local / flash.sh) picks up BOTH halves.
add_custom_command(TARGET tz_nsc_cgc_usb_ns.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} --remove-section=.option_setting* -O ihex
            $<TARGET_FILE:tz_nsc_cgc_usb.elf> tz_nsc_cgc_usb_secure.hex
    COMMAND ${CMAKE_OBJCOPY} -O ihex
            $<TARGET_FILE:tz_nsc_cgc_usb_ns.elf> tz_nsc_cgc_usb_ns.hex
    COMMAND ${RA_REPO_ROOT}/scripts/utils/merge_ihex.py
            tz_nsc_cgc_usb_secure.hex tz_nsc_cgc_usb_ns.hex tz_nsc_cgc_usb.hex
    VERBATIM
    COMMENT "Merging Secure + Non-Secure images into tz_nsc_cgc_usb.hex")

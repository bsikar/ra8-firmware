# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake for `tz_nsc_cgc_usb` -- a TWO-PROJECT TrustZone build (#96).
#
# The Secure ELF (ra8_add_app) holds the secure boot + the NSC CGC veneers and
# emits a CMSE import library (--out-implib). A SEPARATE Non-Secure executable
# (tz_nsc_cgc_usb_ns.elf) links ns_main + ThreadX (+ USBX/ra8_usb later) against
# that import library and its own private libc/libgcc, placed at the NS
# addresses. The two images are merged into one flashable hex. NS->Secure calls
# go only through the import library's SG veneers.
#
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level
# listfile, which CMake requires to contain a literal, direct project()
# call. The aggregate repo-root build instead pulls this app in via
# add_subdirectory() (project() is already called at the root), so guard on
# PARENT_DIRECTORY to avoid a redundant nested sub-project. ra8_add_app()
# below re-runs project() in the standalone case to apply the shared VERSION
# and the app description.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(tz_nsc_cgc_usb LANGUAGES C ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

get_directory_property(_ra8_p PARENT_DIRECTORY)
if(NOT _ra8_p)
  option(RA8_TRUSTZONE_ENABLE "Enable Cortex-M85 TrustZone (-mcmse + SAU)" ON)
endif()

# ---- Secure ELF -----------------------------------------------------------
# Secure boot + NSC CGC veneers. ns_main / ThreadX are NOT here -- they are the
# Non-Secure executable below.
ra8_add_app(
  NAME tz_nsc_cgc_usb BOOT_PROFILE ns_usb_handoff
  STACK_BYTES 2200
  DESCRIPTION "TrustZone two-project: Secure side (CGC veneers + NS boot)"
              AUX_SRCS src/ns_main.c src/ns_usb.c src/ns_usb_host.c
  LIBS ra8_tz_secure_boot
  NSC_SRCS ra8_nsc_cgc.c MRAM_LENGTH 512K
)

# The Secure side must be built for TrustZone, not merely configured for it.
# RA8_TRUSTZONE_ENABLE is the switch ra8_add_app() reads to add -mcmse and the
# RA8_TRUSTZONE_ENABLE define, and it defaults OFF at the repo root
# (CMakeLists.txt) because 234 of the 238 apps are single-image. The option()
# above only fires in a STANDALONE configure of this directory; in the
# aggregate repo build the root has already declared the option, so this app
# inherited OFF, ra8_nsc_cgc.c compiled with no cmse_nonsecure_entry
# functions, and the Secure link died:
#
#   ld: ...tz_nsc_cgc_usb_cmse_import.o: no symbol found for import library
#   ld: ...tz_nsc_cgc_usb_cmse_import.o: failed to generate import library
#   ld: final link failed: no symbols
#
# The other dual-image apps never hit this because they set the define and
# -mcmse on the target directly rather than relying on the option. Do the same
# here: this app IS the two-project TrustZone build (#96), so TrustZone is not
# optional for it whichever way it is configured.
include(${RA8_REPO_ROOT}/libs/ra8_board_ek_ra8d2/ld/ns_memory_map.cmake)
# The Secure boot copies/jumps to the NS image, so it needs the NS addresses as
# constants. They come from the same map the NS linker script is generated from
# rather than being re-typed in trustzone_init.c (#759 item 2).
ra8_ns_memory_map_defines(tz_nsc_cgc_usb.elf)

target_compile_definitions(tz_nsc_cgc_usb.elf PRIVATE RA8_TRUSTZONE_ENABLE)
target_compile_options(tz_nsc_cgc_usb.elf PRIVATE -mcmse)
target_link_options(tz_nsc_cgc_usb.elf PRIVATE -mcmse)

# ---- Post-build: verify the NSC SG-veneer slot offsets (T5-08) -------------
# The NS image reaches each NSC CGC veneer by NAME, through the CMSE import
# library emitted above (--out-implib), which binds ra8_nsc_cgc_* to the
# .gnu.sgstubs addresses. Those byte offsets are the ABI: a reordering of the
# stubs at link time would silently redirect NS->Secure calls without any
# diagnostic. check_sg_offsets.py reads the FINAL Secure symbol table and fails
# the build if any veneer drifted from its expected offset (0/8/16); it
# self-skips a non-TrustZone link. This makes the invariant the linker_script.ld
# comment already claims -- previously unenforced anywhere -- actually hold on
# every build (including CI build-cross).
# --nm pins the SAME cross nm as the link (CMAKE_NM from the toolchain file).
add_custom_command(
  TARGET tz_nsc_cgc_usb.elf
  POST_BUILD
  COMMAND ${RA8_REPO_ROOT}/scripts/checks/check_sg_offsets.py $<TARGET_FILE:tz_nsc_cgc_usb.elf> --nm
          ${CMAKE_NM}
  VERBATIM
  COMMENT "check_sg_offsets: verifying NSC SG-veneer slot offsets (T5-08)"
)

# ---- Non-Secure executable ------------------------------------------------
include(${_d}/cmake/threadx_ns.cmake)

# ra8_add_ns_image() owns the CMSE implib handshake at both ends, the NS
# warning profile, the build ordering, and the merged-hex staple (#759 item 1).
# The USBX and ra8_usb sources this image also needs are added to the target it
# creates, below.
#   RA8_PERIPH_NS_ALIAS -- ra8_usb/ra8_mstp reach the controllers through the IDAU
#                         bit[28]=1 Non-secure alias (0x5025_0000 / 0x5020_3000).
#   RA8_USB_POLLED_ONLY -- ux_dcd_ra8_usb skips the Secure-attributed USB NVIC line;
#                         the worker drives the controller via ra8_usb_dispatch.
ra8_add_ns_image(
  SECURE_TARGET
  tz_nsc_cgc_usb.elf
  NAME
  tz_nsc_cgc_usb_ns
  STACK_BYTES
  2200
  IMPLIB
  ${CMAKE_CURRENT_BINARY_DIR}/tz_nsc_cgc_usb_cmse_import.o
  MERGED_HEX
  tz_nsc_cgc_usb.hex
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ns_main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ns_usb.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ns_usb_host.c
  DEFINES
  RA8_TRUSTZONE_ENABLE
  RA8_PERIPH_NS_ALIAS
  RA8_USB_POLLED_ONLY
  INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/inc
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${RA8_REPO_ROOT}/libs/ra8_core/inc
  ${RA8_REPO_ROOT}/libs/ra8_hal/inc
  ${RA8_REPO_ROOT}/libs/ra8_nsc/inc
  LINK_LIBS
  threadx_ns
)

# ---- USBX + ra8_usb, compiled NS-private into the NS ELF (#96 M2) -----------
# USBX core + CDC-ACM, the ra8_usb<->USBX DCD bridge, the ra8_usb register driver
# and the HAL modules ra8_usb pulls in (ra8_mstp/ra8_log/ra8_time/ra8_usbhs). These
# are compiled FRESH into the NS image so it has its own copies -- the Secure
# side uses ra8_log/ra8_mstp/ra8_time too, and a shared single-ELF copy would
# fault INVTRAN across the S/NS boundary.
set(_ra8_usbx_root ${RA8_REPO_ROOT}/libs/third_party/usbx)
file(GLOB _ns_usbx_core CONFIGURE_DEPENDS "${_ra8_usbx_root}/common/core/src/*.c")
list(
  FILTER
  _ns_usbx_core
  EXCLUDE
  REGEX
  ".*/ux_(dcd_sim_slave|hcd_sim_host)_.*\\.c$"
)
file(GLOB _ns_usbx_cdc CONFIGURE_DEPENDS
     "${_ra8_usbx_root}/common/usbx_device_classes/src/ux_device_class_cdc_acm_*.c"
)

# ra8_time.c is intentionally NOT linked: its ra8_time_init reprograms the SysTick
# ThreadX owns, and its delay path waits on a tick the ThreadX SysTick handler
# never bumps. ns_usb.c provides a ThreadX-backed ra8_delay_ms instead (the only
# ra8_time symbol ra8_usb references).
# ux_dcd_ra8_usb.c and ra8_usb.c were each split into sub-1000-line TUs for the
# file-size cap. Glob the ux_dcd_ra8_usb*.c family (the DCD split: ep / xfer /
# isr / setup / dvst / dvst_default / irq) and list the seven ra8_usb*.c TUs of
# the register driver explicitly (NOT a glob -- the NS world deliberately
# excludes the ra8_usb_pmsc/hmsc/cdc/... class drivers).
file(GLOB _ns_ux_dcd CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/port/usbx/src/ux_dcd_ra8_usb*.c)
set(_ns_usb_srcs
    ${_ns_usbx_core}
    ${_ns_usbx_cdc}
    ${_ns_ux_dcd}
    ${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_usb.c
    ${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_usb_phy.c
    ${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_usb_device.c
    ${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_usb_xfer.c
    ${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_usb_irq.c
    ${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_usb_host_ctrl.c
    ${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_usb_host_bulk.c
    ${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_mstp.c
    ${RA8_REPO_ROOT}/libs/ra8_core/src/ra8_log.c
    ${RA8_REPO_ROOT}/libs/ra8_core/src/ra8_scb.c
)
target_sources(tz_nsc_cgc_usb_ns.elf PRIVATE ${_ns_usb_srcs})
# Quiet only the vendored USBX sources. The first-party USBX bridge, HAL, and
# core sources remain on the full project warning profile. Measured on all 187
# USBX TUs this app compiles, under arm-none-eabi-gcc 13.3.1 at -O0, one flag
# removed at a time: -Wdiscarded-qualifiers and -Wcast-align both fire;
# -Wno-redundant-decls fired on none of the 187 and is deleted. (The app-wide
# -Wno-error=redundant-decls in cmake/ra8_add_app.cmake is a separate control
# and is not what made this one dead -- the class simply never occurs at this
# vendor pin.)
set(_ra8_tz_usbx_warnings
    -Wno-discarded-qualifiers # USBX drops qualifiers through its legacy ULONG/VOID ABI.
    -Wno-cast-align # USBX casts byte-buffer cursors to wider protocol-word pointers.
)
set_source_files_properties(
  ${_ns_usbx_core} ${_ns_usbx_cdc} PROPERTIES COMPILE_OPTIONS "${_ra8_tz_usbx_warnings}"
)
# Vendored USBX headers are not clean under the first-party warning profile
# (e.g. -Wredundant-decls in ux_device_stack.h); SYSTEM keeps the profile on
# the first-party TUs that include them without -Werror-ing SOUP headers.
target_include_directories(
  tz_nsc_cgc_usb_ns.elf SYSTEM
  PRIVATE ${_ra8_usbx_root}/common/core/inc ${_ra8_usbx_root}/common/usbx_device_classes/inc
          ${_ra8_usbx_root}/ports/cortex_m33/gnu/inc
)
target_include_directories(
  tz_nsc_cgc_usb_ns.elf
  PRIVATE ${RA8_REPO_ROOT}/port/usbx/inc ${RA8_REPO_ROOT}/libs/ra8_usb_pal/inc
          ${RA8_REPO_ROOT}/libs/ra8_board_ek_ra8d2/inc
)

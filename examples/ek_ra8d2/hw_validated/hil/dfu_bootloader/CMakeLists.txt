# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMakeLists for dfu_bootloader (immutable USB-DFU MRAM bootloader).
#
# The first 128 KiB of code-MRAM. At reset it boots the valid application slot
# (software A/B select + jump) or, when no slot is valid (or a trigger is set),
# brings up the USBX DFU device on USB-FS and programs the inactive slot. Uses
# the controller-agnostic libs/ra8_dfu core.
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
  project(dfu_bootloader LANGUAGES C ASM)
endif()
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")
ra8_add_app(
  NAME dfu_bootloader
  STACK_BYTES 32768
  DESCRIPTION
    "Immutable USB-DFU MRAM bootloader: A/B slot boot + jump, or DFU the inactive slot over USB-FS"
  USES threadx usbx
  LIBS ra8_dfu_device ra8_dfu ra8_psa_crypto ra8_rot_launch ra8_rot
       THREADX_HEAP
       SDRAM
       MRAM_LENGTH
       128K
       SRAM_TEXT
       ra8_flash.c
)

# --- Root of Trust (RA8_ENABLE_ROOT_OF_TRUST): authenticate every launched slot --
# The copy-to-run launch gate (ra8_dfu_launch) verifies the slot's ECDSA-P256 +
# SHA-256 signature and the extra-MRAM anti-rollback counter BEFORE copying the
# image to the SRAM run base -- default-deny on any failure. The verifier and
# the anti-rollback counter are Zig and arrive as the ra8_rot
# archive named in LIBS above; the gate itself is Zig too and arrives
# as ra8_rot_launch, and ra8_rsip (software SHA-256) via ra8_hal. This block
# links the real tf-psa-crypto backend through the shared recipe in
# cmake/tf_psa_crypto.cmake (the one secure_boot_hil uses to prove the
# mechanism on silicon), adds the p256-m ECDSA driver, and turns the gate on.
# The bootloader's src/psa_verify_rng.c provides a FAIL-CLOSED external-RNG
# hook (verify draws no randomness).
include(${_d}/cmake/tf_psa_crypto.cmake)
ra8_add_tf_psa_crypto(
  tfpsa_dfu
  CONFIG "tf_psa_crypto_config_dfu.h"
  CONFIG_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/inc
  P256M
)

target_link_libraries(dfu_bootloader.elf PRIVATE tfpsa_dfu)
target_compile_definitions(dfu_bootloader.elf PRIVATE RA8_ENABLE_ROOT_OF_TRUST)
target_include_directories(
  dfu_bootloader.elf PRIVATE ${_d}/libs/ra8_psa_crypto/inc ${_d}/libs/ra8_dfu/inc
)

# The tf-psa include directories are SYSTEM, so its header diagnostics do not
# weaken the full project warning profile on the first-party app and RNG shim.

#
# Per-app CMake stub for `cache_coherency_hil`. The Cortex-M85 (CPU0) image is
# built by the shared cmake/ra8_add_app.cmake recipe with the cache + MPU boot
# path turned ON; the Cortex-M33 (CPU1) responder image is built + embedded by
# the shared ra8_add_cpu1_image() helper, so one SWD flash drops both cores'
# images. This is the dual-core analog of dma_coherency_hil: it proves the
# M85<->M33 hand-off stays coherent with the D-cache enabled because the shared
# struct lives in the boot's non-cacheable MPU region 4 (0x22100000).
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level listfile,
# which CMake requires to contain a literal, direct project() call. The aggregate
# repo-root build instead pulls this app in via add_subdirectory() (project() is
# already called at the root), so guard on PARENT_DIRECTORY to avoid a redundant
# nested sub-project. ra8_add_app() below re-runs project() in the standalone case
# to apply the shared VERSION and the app description.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(cache_coherency_hil LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME cache_coherency_hil
  STACK_BYTES 2200
  DESCRIPTION
    "M85<->M33 cache-coherency HIL validator (D-cache ON, non-cacheable shared SRAM) for RA8D2"
)

# Turn ON the Cortex-M85 D-cache / I-cache + MPU boot path for THIS app only.
# The shared boot (libs/ra8_board_ek_ra8d2/boot/system_init.c) is compiled
# per-app, so this define reaches only cache_coherency_hil.elf. With the D-cache
# enabled, the M85<->M33 hand-off through SRAM is coherent ONLY because the boot
# maps the shared bank (0x22100000) as a non-cacheable MPU region -- which is the
# whole point of this HIL test. The define is NOT applied to the CPU1 image
# (the Cortex-M33 has no cache and runs its own minimal boot).
target_compile_definitions(cache_coherency_hil.elf PRIVATE RA8_BOOT_ENABLE_CACHE_MPU)

# Keep INFO-level logging in this RelWithDebInfo HIL build so the PASS result is
# also emitted over ITM (additive to the VCOM banner), which board_sim surfaces
# on its [itm] tap.
target_compile_definitions(cache_coherency_hil.elf PRIVATE RA8_LOG_LEVEL=RA8_LOG_LEVEL_INFO)

#############################################################################
# Second (Cortex-M33 / CPU1) responder image, embedded into the M85 ELF.
# The shared cmake/ra8_add_cpu1_image() helper builds cpu1_main.c as a
# freestanding M33 executable and links its .cpu1_image blob into the M85 ELF;
# linker_script.ld pins .cpu1_image at ORIGIN(MRAM_CPU1) (0x020C0000).
#############################################################################
ra8_add_cpu1_image(
  PARENT cache_coherency_hil
  NAME cache_coherency_hil_cpu1
  SOURCES cpu1_main.c
)

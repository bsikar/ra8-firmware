#
# Per-app CMake stub for `ereader_shelf`. All build logic lives in the shared
# cmake/ra_add_app.cmake recipe; this file only names the app + its deps.
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
    project(ereader_shelf LANGUAGES C CXX ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra_add_app.cmake")

# Hybrid baked+SD e-reader: ra_box grid layout + ra_ui geometry draw through
# ra_gfx into the SDRAM framebuffer ra_display_pal scans over GLCDC to the
# 1024x600 panel. ra_book + miniz inflate the compressed .rabook blobs (baked in
# MRAM via library.h, or read from SD). src/*.c hold the per-screen modules.
ra_add_app(NAME ereader_shelf STACK_BYTES 16384
           DESCRIPTION "Hybrid baked+SD e-reader: shelf, covers, TOC, full reader (1024x600 panel)"
           LIBS ra_gfx ra_display_pal ra_box ra_ui ra_book ra_fs ra_sdmmc_spi ra_epub ra_reflow
                ra_board_ek_ra8d2 ra_io_bus)

# Pin the reader's optimisation level to -Og on this target, independent of the
# configured build type. Two facts force this:
#   * At -O0 the fully-linked reader (ra_epub + miniz + tinyxml2 + stb +
#     ra_reflow + baked library.h ...) overflows the 1 MiB MRAM code region by
#     ~17 KiB, so the top-level aggregate build (which defaults to Debug/-O0)
#     and `make sim-ereader_shelf` cannot link it. A per-target option lands
#     after the build-type -O flag, so -Og wins (GCC honours the last -O) and
#     the app builds regardless of how it was configured.
#   * -O1 and above miscompile the shelf layout under this toolchain: the header
#     bar shifts, ~63% of framebuffer pixels change, boot balloons ~3400x in the
#     emulator, and -O2's auto-vectorised byte-fill (vmov.i8) is an MVE encoding
#     the board_sim CPU model cannot execute. The result fails the board_sim
#     framebuffer-hash HIL gate (hil.conf `fb=FA3AB5B5`) and cannot be
#     bench-validated from here. Shipping >-Og is deferred to a focused
#     follow-up (recon theme T2): root-cause the optimisation-sensitive layout
#     divergence (latent UB in the ra_box/ra_ui/sh_shelf geometry vs a board_sim
#     instruction-emulation gap) and, once proven on silicon, re-baseline the
#     golden or extend the sim.
# -Og is the highest level the board_sim golden gate validates today; it renders
# byte-identically to the pinned `fb=FA3AB5B5` reference.
target_compile_options(ereader_shelf.elf PRIVATE -Og)

# Turn on the shared boot's Cortex-M85 L1 cache + MPU bring-up for this app. The
# shared SystemInit (libs/ra_board_ek_ra8d2/boot/system_init.c) is compiled into
# each app's own executable, so this PRIVATE define reaches that TU here and
# nowhere else: it programmes the 5-region MPU and enables the I-cache, D-cache,
# and branch predictor before main() runs (substrate silicon-validated by
# cache_mpu_hil / cache_coherency_hil / dma_coherency_hil). The reader executes
# and renders out of high-latency MRAM / SDRAM, so uncached it stalls the 1 GHz
# core on nearly every fetch; this is the single largest CPU-active-time win.
#
# The same define also arms the framebuffer D-cache clean in ra_display_pal_lcd
# (lcd_flush / lcd_clear): the framebuffer lives in cacheable SDRAM and the GLCDC
# scans it over AXI without seeing the L1 D-cache, so each flush cleans the
# painted rows back to SDRAM before the next scan. The SD path is polled SCI
# Simple-SPI (CPU PIO, no DMA master) and needs no maintenance, and the app links
# no ra_drw, so the framebuffer is the only coherency boundary.
#
# board_sim has no cache model, so it renders identically with the flag on
# (validated: fb=FA3AB5B5 unchanged). The cache CORRECTNESS itself -- that the
# clean prevents stale scan-out on real silicon with the D-cache live -- is
# HIL-only and remains bench-validation-pending for this app.
target_compile_definitions(ereader_shelf.elf PRIVATE RA_BOOT_ENABLE_CACHE_MPU)

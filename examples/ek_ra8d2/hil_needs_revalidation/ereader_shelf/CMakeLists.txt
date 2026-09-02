# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `ereader_shelf`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app + its deps.
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
  project(ereader_shelf LANGUAGES C CXX ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# Hybrid baked+SD e-reader: ra8_box grid layout + ra8_ui geometry draw through
# ra8_gfx into the SDRAM framebuffer ra8_display_pal scans over GLCDC to the
# 1024x600 panel. book + miniz inflate the compressed .rabook blobs (baked in
# MRAM via library.h, or read from SD). src/*.c hold the per-screen modules.
# comic opens CBZ (ZIP) + CBR (RAR) comic archives and streams page images
# for the full-page comic reader (sh_comic.c, #236); it reuses epub's
# miniz-with-ZIP + static pool, already a dep here.
ra8_add_app(
  NAME ereader_shelf
  STACK_BYTES 16384
  DESCRIPTION
    "Hybrid baked+SD e-reader: shelf, covers, TOC, full reader + CBZ/CBR comics (1024x600 panel)"
  LIBS ra8_gfx
       ra8_display_pal
       ra8_box
       ra8_ui
       book
       ra8_fs
       ra8_sdmmc_spi
       epub
       reflow
       comic
       ra8_board_ek_ra8d2
       ra8_io_bus
)

# Pin the reader's optimisation level to -Og on this target, independent of the
# configured build type. Two facts force this:
#   * At -O0 the fully-linked reader (epub + miniz + bounded XML reader + stb +
#     reflow + baked library.h ...) overflows the 1 MiB MRAM code region by
#     ~17 KiB, so the top-level aggregate build (which defaults to Debug/-O0)
#     and `just apps::emulator::run ereader_shelf` cannot link it. A per-target option lands
#     after the build-type -O flag, so -Og wins (GCC honours the last -O) and
#     the app builds regardless of how it was configured.
#   * The pin is a ra8_emulator GATE-SPEED choice, not a firmware limit. The
#     firmware is optimisation-stable: the identical source renders byte-
#     identical at -O0/-O1/-O2/-O3 on the host (clang + gcc), UBSan- and ASan-
#     clean (guarded by apps/board/stand_alone/ereader/tests/src/test_ereader_shelf_geometry.c),
#     and -- since
#     ra8_emulator's LOB decode was fixed (#233) -- ra8_emulator now renders the -O1
#     reader byte-identically to -Og too (both fb=0ADB62C6). So real M85 silicon
#     (which has the Armv8.1-M LOB + MVE hardware natively) runs the reader at
#     -O1+ with no ra8_emulator relaxation.
#     What -Og buys is SPEED under the emulator: ra8_emulator runs the Cortex-M85
#     image on a Unicorn Cortex-M33 that lacks the v8.1-M instructions GCC emits
#     at higher -O (at -O1 the ra8_gfx fill path internal_fill_rect_565 becomes a
#     Low-Overhead-Branch `dls`/`le` loop; -O2 auto-vectorises to MVE). ra8_emulator
#     hand-emulates these via trap-and-relaunch -- one emulation chunk PER loop
#     iteration -- so an -O1 render grinds the reader's counted loops through
#     ~2.5M relaunches (tens of seconds), while -Og emits no LOB at all and the
#     golden gate (hil.conf fb=0ADB62C6) runs in well under a second. Until the
#     LOB seam batches whole loops, -Og stays the gated level for a fast gate.
# NOTE: the earlier "ra8_emulator has no LOB seam / -O1 is fb=DE69E794 corruption"
# was the pre-#233 state -- ra8_emulator's emulate_lob mis-decoded the LE branch
# offset (the scattered immediate LSB), branching mid-instruction; that bug is
# fixed, so -O1 no longer diverges, only runs slower under emulation.
target_compile_options(ereader_shelf.elf PRIVATE -Og)

# Turn on the shared boot's Cortex-M85 L1 cache + MPU bring-up for this app. The
# shared SystemInit (libs/ra8_board_ek_ra8d2/src/boot/system_init.c) is compiled into
# each app's own executable, so this PRIVATE define reaches that TU here and
# nowhere else: it programmes the 5-region MPU and enables the I-cache, D-cache,
# and branch predictor before main() runs (substrate silicon-validated by
# cache_mpu_hil / cache_coherency_hil / dma_coherency_hil). The reader executes
# and renders out of high-latency MRAM / SDRAM, so uncached it stalls the 1 GHz
# core on nearly every fetch; this is the single largest CPU-active-time win.
#
# The same define also arms the framebuffer D-cache clean in ra8_display_pal_lcd
# (lcd_flush / lcd_clear): the framebuffer lives in cacheable SDRAM and the GLCDC
# scans it over AXI without seeing the L1 D-cache, so each flush cleans the
# painted rows back to SDRAM before the next scan. The SD path is polled SCI
# Simple-SPI (CPU PIO, no DMA initiator) and needs no maintenance, and the app links
# no ra8_drw, so the framebuffer is the only coherency boundary.
#
# ra8_emulator has no cache model, so it renders identically with the flag on
# (validated: fb=FA3AB5B5 unchanged). The cache CORRECTNESS itself -- that the
# clean prevents stale scan-out on real silicon with the D-cache live -- is
# HIL-only and remains bench-validation-pending for this app.
target_compile_definitions(ereader_shelf.elf PRIVATE RA8_BOOT_ENABLE_CACHE_MPU)

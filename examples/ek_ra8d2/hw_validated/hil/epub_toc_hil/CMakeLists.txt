#
# Per-app CMake stub for `epub_toc_hil`. It joins the SD storage stack
# (ra_fs FAT layer over the ra_sdmmc_spi SPI-mode driver) to the EPUB parse
# stack: ra_epub pulls in the vendored miniz (ZIP) + tinyxml2 (XML) via the
# ra_epub special-case in ra_add_app.cmake, ra_reflow supplies the stb_truetype
# glyph glue ra_epub links against, and ra_gfx backs ra_reflow's render path.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra_add_app.cmake")

ra_add_app(NAME epub_toc_hil STACK_BYTES 16384
           DESCRIPTION "EPUB TOC navigation (NCX + nav.xhtml) from SD via ra_fs (#116)"
           LIBS ra_epub ra_reflow ra_gfx ra_fs ra_sdmmc_spi)

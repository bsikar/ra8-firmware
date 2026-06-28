#
# Per-app CMake stub for `compile_on_m33`. The Cortex-M85 (CPU0) image is built
# by the shared cmake/ra_add_app.cmake recipe and links ra_book so it can run
# ra_book_validate on the blob the M33 emits. The Cortex-M33 (CPU1) image is a
# second executable that links the zero-heap RABOOK1 emitter (ra_rabook_compile)
# and is objcopy'd into the M85 ELF so one SWD flash drops both cores' images.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level listfile,
# which CMake requires to contain a literal, direct project() call. The
# aggregate repo-root build instead pulls this app in via add_subdirectory()
# (project() is already called at the root), so guard on PARENT_DIRECTORY to
# avoid a redundant nested sub-project. ra_add_app() below re-runs project() in
# the standalone case to apply the shared VERSION and the app description.
get_directory_property(_ra_has_parent PARENT_DIRECTORY)
if(NOT _ra_has_parent)
    # CXX is required for the CPU1 image: it links the EPUB front end whose XML
    # shim + tinyxml2 are C++ (#149 stage a0). The aggregate repo build already
    # enables CXX at the root; this is only for the standalone `cmake -S .`.
    project(compile_on_m33 LANGUAGES C CXX ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
    get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra_add_app.cmake")

ra_add_app(NAME compile_on_m33 STACK_BYTES 4096
           DESCRIPTION "Dual-core (M85 + M33): the M33 runs the RABOOK1 emitter back-end; the M85 validates the blob (#149b)")

# The M85 image needs only ra_book_validate plus the header-only ra_book.h inline
# accessors, so link the single dependency-clean ra_book.c core directly -- NOT
# the whole ra_book lib via LIBS. ra_book's xhtml / paged TUs carry deep stack
# frames (ra_book_walk_to_xhtml ~4 KiB) and pull in ra_mem, none of which this
# validate-only app exercises; globbing them in would trip -Wstack-usage.
target_sources(compile_on_m33.elf PRIVATE ${RA_REPO_ROOT}/libs/ra_book/src/ra_book.c)
target_include_directories(compile_on_m33.elf PRIVATE ${RA_REPO_ROOT}/libs/ra_book/inc)

#############################################################################
# Second (Cortex-M33 / CPU1) image, embedded into the M85 ELF.
# The shared cmake/ra_add_cpu1_image() helper builds cpu1_main.c as a
# freestanding M33 executable and links its .cpu1_image blob into the M85 ELF;
# linker_script.ld pins .cpu1_image at ORIGIN(MRAM_CPU1) (0x020C0000).
#
# Unlike the header-only ereader_m33 reader, this image actually LINKS the
# zero-heap RABOOK1 emitter (libs/ra_rabook_compile/src/ra_rabook_compile.c),
# so it needs ra_book's and ra_rabook_compile's include dirs. The emitter only
# leans on memcpy/memset/strlen (newlib) plus ra_log/ra_check macros; defining
# RA_LOG_LEVEL=0 collapses every ra_log call to a no-op so no logging backend is
# dragged into the second core (it would otherwise resolve to internal_ra_log_*).
#############################################################################
ra_add_cpu1_image(
    PARENT   compile_on_m33
    NAME     compile_on_m33_cpu1
    SOURCES  cpu1_main.c
    INCLUDES ${RA_REPO_ROOT}/libs/ra_book/inc
             ${RA_REPO_ROOT}/libs/ra_rabook_compile/inc
             ${RA_REPO_ROOT}/libs/ra_epub/inc
             ${RA_REPO_ROOT}/libs/ra_epub/src
             ${RA_REPO_ROOT}/libs/ra_mem/inc
             ${RA_REPO_ROOT}/libs/ra_reflow/inc
             ${RA_REPO_ROOT}/libs/ra_img/inc
             ${RA_REPO_ROOT}/libs/third_party/miniz
             ${RA_REPO_ROOT}/libs/third_party/tinyxml2
             ${RA_REPO_ROOT}/libs/third_party/stb)

# #149 stage a0 -- prove the full text/CSS/SVG EPUB->RABOOK1 back-end (the EPUB
# front end: miniz ZIP + tinyxml2 XHTML, plus the rabook emitter + chapter shim)
# LINKS into the 256 KiB Cortex-M33 image and fits, BEFORE any parser wiring. The
# emitter PoC in cpu1_main.c is unchanged; the back-end is force-kept (below) so
# `size` reflects its real cost. stb_image and ra_fs are deliberately NOT linked:
# raster transcode is excluded (text/CSS/SVG only) and the M33 finalizes into a
# buffer rather than ra_rabook_compile_from_epub (which writes via ra_fs) -- so
# ra_rabook_pipeline.c (which references stb_image) is also held back to a1.
if(TARGET compile_on_m33_cpu1.elf)
    file(GLOB _c1_epub_c   CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_epub/src/*.c)
    file(GLOB _c1_epub_cpp CONFIGURE_DEPENDS ${RA_REPO_ROOT}/libs/ra_epub/src/*.cpp)
    target_sources(compile_on_m33_cpu1.elf PRIVATE
        ${RA_REPO_ROOT}/libs/ra_rabook_compile/src/ra_rabook_compile.c
        ${RA_REPO_ROOT}/libs/ra_rabook_compile/src/ra_rabook_gray4.c
        ${RA_REPO_ROOT}/libs/ra_rabook_compile/src/ra_rabook_xml_shim.cpp
        ${_c1_epub_c}
        ${_c1_epub_cpp}
        ${RA_REPO_ROOT}/libs/third_party/miniz/miniz.c
        ${RA_REPO_ROOT}/libs/third_party/tinyxml2/tinyxml2.cpp)
    # MINIZ_NO_STDIO/NO_TIME on EVERY TU that includes miniz.h (miniz.c + ra_epub)
    # so the header ABI matches and miniz.c's fopen/utime file path (absent on
    # bare-metal newlib) is dropped -- ra_epub uses only the in-memory reader.
    target_compile_definitions(compile_on_m33_cpu1.elf PRIVATE
        RA_LOG_LEVEL=0 MINIZ_NO_STDIO MINIZ_NO_TIME)
    # The vendored miniz.c / tinyxml2.cpp cannot satisfy the first-party -Werror
    # set; -w them (the first-party ra_epub/rabook .c/.cpp keep the full gate).
    # miniz ALSO needs the same -fno-strict-aliasing the M85 SOUP build uses, or
    # the M33 blob diverges from the byte-identical M85 path (a prior host
    # divergence was exactly miniz strict-aliasing UB under -Os).
    set_property(SOURCE ${RA_REPO_ROOT}/libs/third_party/miniz/miniz.c
        APPEND PROPERTY COMPILE_OPTIONS -w -fno-strict-aliasing)
    set_property(SOURCE ${RA_REPO_ROOT}/libs/third_party/tinyxml2/tinyxml2.cpp
        APPEND PROPERTY COMPILE_OPTIONS -w)
    # Force-keep the parser/emitter entry points the linker would otherwise
    # --gc-sections away (cpu1_main.c only calls the emitter PoC), so the .map
    # measures the real text/CSS/SVG back-end cost against the 256 KiB budget.
    target_link_options(compile_on_m33_cpu1.elf PRIVATE
        -Wl,--undefined=ra_epub_open
        -Wl,--undefined=ra_rabook_xml_parse_chapter
        -Wl,--undefined=ra_rabook_finalize)
endif()

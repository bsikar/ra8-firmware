#
# Per-app CMake stub for `compile_on_m33`. The Cortex-M85 (CPU0) image is built
# by the shared cmake/ra8_add_app.cmake recipe and links ra8_book so it can run
# ra8_book_validate on the blob the M33 emits. The Cortex-M33 (CPU1) image is a
# second executable that links the zero-heap RABOOK1 emitter (ra8_rabook_compile)
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
# avoid a redundant nested sub-project. ra8_add_app() below re-runs project() in
# the standalone case to apply the shared VERSION and the app description.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  # CXX is required for the CPU1 image: it links the EPUB front end whose XML
  # shim + tinyxml2 are C++ (#149 stage a0). The aggregate repo build already
  # enables CXX at the root; this is only for the standalone `cmake -S .`.
  project(compile_on_m33 LANGUAGES C CXX ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME compile_on_m33
  STACK_BYTES 4096
  DESCRIPTION "Dual-core M85 + M33: M33 runs the RABOOK1 emitter, M85 validates (#149b)"
)

# The M85 image needs only ra8_book_validate plus the header-only ra8_book.h inline
# accessors, so link the single dependency-clean ra8_book.c core directly -- NOT
# the whole ra8_book lib via LIBS. ra8_book's xhtml / paged TUs carry deep stack
# frames (ra8_book_walk_to_xhtml ~4 KiB) and pull in ra8_mem, none of which this
# validate-only app exercises; globbing them in would trip -Wstack-usage.
target_sources(compile_on_m33.elf PRIVATE ${RA8_REPO_ROOT}/libs/ra8_book/src/ra8_book.c)
target_include_directories(compile_on_m33.elf PRIVATE ${RA8_REPO_ROOT}/libs/ra8_book/inc)

#############################################################################
# Second (Cortex-M33 / CPU1) image, embedded into the M85 ELF.
# The shared cmake/ra8_add_cpu1_image() helper builds cpu1_main.c as a
# freestanding M33 executable and links its .cpu1_image blob into the M85 ELF;
# linker_script.ld pins .cpu1_image at ORIGIN(MRAM_CPU1) (0x020C0000).
#
# Unlike the header-only ereader_m33 reader, this image actually LINKS the
# zero-heap RABOOK1 emitter (libs/ra8_rabook_compile/src/ra8_rabook_compile.c),
# so it needs ra8_book's and ra8_rabook_compile's include dirs. The emitter only
# leans on memcpy/memset/strlen (newlib) plus ra8_log/ra8_check macros; defining
# RA8_LOG_LEVEL=0 collapses every ra8_log call to a no-op so no logging backend is
# dragged into the second core (it would otherwise resolve to internal_ra8_log_*).
#############################################################################
ra8_add_cpu1_image(
  PARENT compile_on_m33
  NAME compile_on_m33_cpu1
  SOURCES cpu1_main.c
  INCLUDES ${RA8_REPO_ROOT}/libs/ra8_book/inc
           ${RA8_REPO_ROOT}/libs/ra8_hal/inc
           ${RA8_REPO_ROOT}/libs/ra8_rabook_compile/inc
           ${RA8_REPO_ROOT}/libs/ra8_epub/inc
           ${RA8_REPO_ROOT}/libs/ra8_epub/src
           ${RA8_REPO_ROOT}/libs/ra8_mem/inc
           ${RA8_REPO_ROOT}/libs/ra8_reflow/inc
           ${RA8_REPO_ROOT}/libs/ra8_img/inc
           ${RA8_REPO_ROOT}/libs/ra8_fs/inc
           ${RA8_REPO_ROOT}/libs/third_party/miniz
           ${RA8_REPO_ROOT}/libs/third_party/tinyxml2
           ${RA8_REPO_ROOT}/libs/third_party/stb
)

# #149 stage a1 -- the M33 runs the REAL text/CSS/SVG EPUB->RABOOK1 compile. It
# links the EPUB front end (miniz ZIP + tinyxml2 XHTML) plus the rabook pipeline
# and emitter, and cpu1_main.c calls ra8_rabook_compile_from_epub_to_buffer over
# the baked parity fixture (no filesystem -- it finalizes into the shared SRAM2
# buffer). RA8_RABOOK_NO_RASTER compiles out the stb_image raster path (the
# fixture cover is an SVG, stored verbatim), so stb is NOT linked; the FS-writing
# ra8_rabook_compile_from_epub is unreferenced and --gc-sections drops it, so
# ra8_fs.c is not linked either (only ra8_fs.h is on the include path).
if(TARGET compile_on_m33_cpu1.elf)
  file(GLOB _c1_epub_c CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_epub/src/*.c)
  file(GLOB _c1_epub_cpp CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/ra8_epub/src/*.cpp)
  target_sources(
    compile_on_m33_cpu1.elf
    PRIVATE ${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_ipc.c
            # ra8_epub now guards its ZIP entries through the shared decompression
            # limits policy (ra8_epub_zip_guard.c -> ra8_decomp_limits_*); the M33
            # image hand-picks its sources, so link the core policy TU explicitly.
            ${RA8_REPO_ROOT}/libs/ra8_core/src/ra8_decomp_limits.c
            ${RA8_REPO_ROOT}/libs/ra8_rabook_compile/src/ra8_rabook_compile.c
            ${RA8_REPO_ROOT}/libs/ra8_rabook_compile/src/ra8_rabook_gray4.c
            ${RA8_REPO_ROOT}/libs/ra8_rabook_compile/src/ra8_rabook_xml_shim.cpp
            ${RA8_REPO_ROOT}/libs/ra8_rabook_compile/src/ra8_rabook_pipeline.c
            ${_c1_epub_c}
            ${_c1_epub_cpp}
            ${RA8_REPO_ROOT}/libs/third_party/miniz/miniz.c
            ${RA8_REPO_ROOT}/libs/third_party/tinyxml2/tinyxml2.cpp
  )
  # MINIZ_NO_STDIO/NO_TIME on EVERY TU that includes miniz.h (miniz.c + ra8_epub)
  # so the header ABI matches and miniz.c's fopen/utime file path (absent on
  # bare-metal newlib) is dropped -- ra8_epub uses only the in-memory reader.
  target_compile_definitions(
    compile_on_m33_cpu1.elf PRIVATE RA8_LOG_LEVEL=0 MINIZ_NO_STDIO MINIZ_NO_TIME
                                    RA8_RABOOK_NO_RASTER
  )
  # The vendored miniz.c / tinyxml2.cpp cannot satisfy the first-party -Werror
  # set; -w them (the first-party ra8_epub/rabook .c/.cpp keep the full gate).
  # miniz ALSO needs the same -fno-strict-aliasing the M85 SOUP build uses, or
  # the M33 blob diverges from the byte-identical M85 path (a prior host
  # divergence was exactly miniz strict-aliasing UB under -Os).
  set_property(
    SOURCE ${RA8_REPO_ROOT}/libs/third_party/miniz/miniz.c
    APPEND
    PROPERTY COMPILE_OPTIONS -w -fno-strict-aliasing
  )
  set_property(
    SOURCE ${RA8_REPO_ROOT}/libs/third_party/tinyxml2/tinyxml2.cpp
    APPEND
    PROPERTY COMPILE_OPTIONS -w
  )
  # Force-keep the parser/emitter entry points the linker would otherwise
  # --gc-sections away (cpu1_main.c only calls the emitter PoC), so the .map
  # measures the real text/CSS/SVG back-end cost against the 256 KiB budget.
  target_link_options(
    compile_on_m33_cpu1.elf
    PRIVATE
    -Wl,--undefined=ra8_epub_open
    -Wl,--undefined=ra8_rabook_xml_parse_chapter
    -Wl,--undefined=ra8_rabook_finalize
  )
endif()

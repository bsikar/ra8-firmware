# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `compile_on_m33`. The Cortex-M85 (CPU0) image is built
# by the shared cmake/ra8_add_app.cmake recipe and links book so it can run
# book_validate on the blob the M33 emits. The Cortex-M33 (CPU1) image is a
# second executable that links the zero-heap RABOOK1 emitter (rabook_compile)
# and is objcopy'd into the M85 ELF so one SWD flash drops both cores' images.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
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
  project(compile_on_m33 LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME compile_on_m33
  STACK_BYTES 4096 AUX_SRCS src/cpu1_main.c
  DESCRIPTION "Dual-core M85 + M33: M33 runs the RABOOK1 emitter, M85 validates (#149b)"
)

# The M85 image needs only book_validate plus the header-only book.h inline
# accessors, so link the single dependency-clean book.c core directly -- NOT
# the whole book lib via LIBS. book's xhtml / paged TUs carry deep stack
# frames (book_walk_to_xhtml ~4 KiB) and pull in ra8_mem, none of which this
# validate-only app exercises; globbing them in would trip -Wstack-usage.
target_sources(compile_on_m33.elf PRIVATE ${RA8_REPO_ROOT}/apps/shared_libs/book/src/book.c)
target_include_directories(compile_on_m33.elf PRIVATE ${RA8_REPO_ROOT}/apps/shared_libs/book/inc)

#############################################################################
# Second (Cortex-M33 / CPU1) image, embedded into the M85 ELF.
# The shared cmake/ra8_add_cpu1_image() helper builds cpu1_main.c as a
# freestanding M33 executable and links its .cpu1_image blob into the M85 ELF;
# linker_script.ld pins .cpu1_image at ORIGIN(MRAM_CPU1) (0x020C0000).
#
# Unlike the header-only ereader_m33 reader, this image actually LINKS the
# zero-heap RABOOK1 emitter (apps/shared_libs/rabook_compile/src/rabook_compile.c),
# so it needs book's and rabook_compile's include dirs. The emitter only
# leans on memcpy/memset/strlen (newlib) plus ra8_log/ra8_check macros; defining
# RA8_LOG_LEVEL=0 collapses every ra8_log call to a no-op so no logging backend is
# dragged into the second core (it would otherwise resolve to ra8_log_emit_*).
#############################################################################
ra8_add_cpu1_image(
  PARENT compile_on_m33
  NAME compile_on_m33_cpu1
  SOURCES src/cpu1_main.c
  INCLUDES ${RA8_REPO_ROOT}/apps/shared_libs/book/inc
           ${RA8_REPO_ROOT}/libs/ra8_hal/inc
           ${RA8_REPO_ROOT}/apps/shared_libs/rabook_compile/inc
           ${RA8_REPO_ROOT}/apps/shared_libs/epub/inc
           ${RA8_REPO_ROOT}/apps/shared_libs/epub/src
           ${RA8_REPO_ROOT}/libs/ra8_mem/inc
           ${RA8_REPO_ROOT}/apps/shared_libs/reflow/inc
           ${RA8_REPO_ROOT}/libs/ra8_fs/inc
           ${RA8_REPO_ROOT}/apps/shared_libs/xml/inc
           ${RA8_REPO_ROOT}/apps/shared_libs/third_party/miniz
           ${RA8_REPO_ROOT}/apps/shared_libs/third_party/stb
)

# #149 stage a1 -- the M33 runs the REAL text/CSS/SVG EPUB->RABOOK1 compile. It
# links the EPUB front end (miniz ZIP + bounded XML reader) plus the rabook pipeline
# and emitter, and cpu1_main.c calls rabook_compile_from_epub_to_buffer over
# the baked parity fixture (no filesystem -- it finalizes into the shared SRAM2
# buffer). RA8_RABOOK_NO_RASTER compiles out the stb_image raster path (the
# fixture cover is an SVG, stored verbatim), so stb is NOT linked; the FS-writing
# rabook_compile_from_epub is unreferenced and --gc-sections drops it, so
# ra8_fs.c is not linked either (only ra8_fs.h is on the include path).
if(TARGET compile_on_m33_cpu1.elf)
  file(GLOB _c1_epub_c CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/apps/shared_libs/epub/src/*.c)
  target_sources(
    compile_on_m33_cpu1.elf
    PRIVATE ${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_ipc.c
            # epub now guards its ZIP entries through the shared decompression
            # limits policy (epub_zip_guard.c -> ra8_decomp_limits_*); the M33
            # image hand-picks its sources, so link the core policy TU explicitly.
            ${RA8_REPO_ROOT}/libs/ra8_core/src/ra8_decomp_limits.c
            ${RA8_REPO_ROOT}/apps/shared_libs/rabook_compile/src/rabook_compile.c
            ${RA8_REPO_ROOT}/apps/shared_libs/rabook_compile/src/ra8_rabook_finalize.c
            ${RA8_REPO_ROOT}/apps/shared_libs/rabook_compile/src/ra8_rabook_gray4.c
            ${RA8_REPO_ROOT}/apps/shared_libs/rabook_compile/src/ra8_rabook_xml_shim.c
            ${RA8_REPO_ROOT}/apps/shared_libs/rabook_compile/src/ra8_rabook_pipeline.c
            ${RA8_REPO_ROOT}/apps/shared_libs/xml/src/xml.c
            ${RA8_REPO_ROOT}/apps/shared_libs/xml/src/xml_decode.c
            ${RA8_REPO_ROOT}/apps/shared_libs/xml/src/xml_doctype.c
            ${_c1_epub_c}
            ${RA8_REPO_ROOT}/apps/shared_libs/third_party/miniz/miniz.c
  )
  # MINIZ_NO_STDIO/NO_TIME on EVERY TU that includes miniz.h (miniz.c + epub)
  # so the header ABI matches and miniz.c's fopen/utime file path (absent on
  # bare-metal newlib) is dropped -- epub uses only the in-memory reader.
  target_compile_definitions(
    compile_on_m33_cpu1.elf PRIVATE RA8_LOG_LEVEL=0 MINIZ_NO_STDIO MINIZ_NO_TIME
                                    RA8_RABOOK_NO_RASTER
  )
  # miniz.c needs the same -fno-strict-aliasing the M85 SOUP build uses, or the
  # M33 blob diverges from the byte-identical M85 path (a prior host divergence
  # was exactly miniz strict-aliasing UB under -Os).
  #
  # It carries NO -Wno flags, deliberately. It used to carry six, under the note
  # "vendored miniz.c cannot satisfy the first-party -Werror set". There is no
  # -Werror set on this target to satisfy: ra8_add_cpu1_image() applies the
  # project warning profile PER-SOURCE to the helper's own SOURCES only, and
  # this app bolts its CPU1 translation units on with target_sources(), so the
  # whole CPU1 compile line here is -mcpu=cortex-m33 ... -Os with no -Wall at
  # all -- for cpu1_main.c too, which is first-party (the gap is the standing
  # TODO(T1-09) in cmake/ra8_add_app.cmake). Compiling miniz.c with all six
  # removed, under arm-none-eabi-gcc 13.3.1 at the -Os this image uses, emits
  # nothing. Six flags that mask nothing are worse than none: they read as
  # evidence that this image is warning-gated when it is not. When T1-09 lands,
  # re-measure and re-add whatever actually fires.
  set_property(
    SOURCE ${RA8_REPO_ROOT}/apps/shared_libs/third_party/miniz/miniz.c
    APPEND
    PROPERTY COMPILE_OPTIONS -fno-strict-aliasing
  )
  # Force-keep the parser/emitter entry points the linker would otherwise
  # --gc-sections away (cpu1_main.c only calls the emitter PoC), so the .map
  # measures the real text/CSS/SVG back-end cost against the 256 KiB budget.
  target_link_options(
    compile_on_m33_cpu1.elf
    PRIVATE
    -Wl,--undefined=epub_open
    -Wl,--undefined=ra8_rabook_xml_parse_chapter
    -Wl,--undefined=ra8_rabook_finalize
  )
endif()

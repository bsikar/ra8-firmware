# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# tools/rabook_imagepack -- single-file converter / inspector for the first-party
# content container formats.
#
# The single-unit counterpart to the batch producers (apps/host/mdl,
# tools/epub_compile): convert ONE file into ONE container, dump a container's
# structure, or round-trip a source to prove the produced bytes are correct.
#
#   cmake -B build -S . && cmake --build build -j
#   ./build/rabook_imagepack convert --format jof --in page.jpg --out page.jof
#   ./build/rabook_imagepack inspect page.jof --verbose
#   ./build/rabook_imagepack verify  --format jof --in page.jpg --out recon.ppm
#
# It links the firmware's own producer and reader (not a host reimplementation),
# so a defect reproduced here is a defect on silicon and a clean result here
# exonerates the file.
#
#

cmake_minimum_required(VERSION 3.20)

project(rabook_imagepack LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Request each hosted libc's descriptor-relative declarations through the
# build contract, rather than defining implementation-reserved feature macros
# in first-party C sources covered by MISRA Rule 21.1.
set(RA8_FMT_HOST_FEATURE_DEFINITIONS)
if(APPLE)
  list(APPEND RA8_FMT_HOST_FEATURE_DEFINITIONS _DARWIN_C_SOURCE)
elseif(UNIX)
  list(APPEND RA8_FMT_HOST_FEATURE_DEFINITIONS _GNU_SOURCE)
endif()

# Firmware root (two directories up from tools/rabook_imagepack/).
get_filename_component(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# libs/ra8_imgdec/inc/ra8_imgdec*.h is Zig now, so the targets below that used
# to compile libs/ra8_imgdec/src/*.c by path link the archive instead.
if(NOT TARGET ra8_zig::ra8_imgdec)
  include(${FW_ROOT}/tests/cmake/zig_library.cmake)
  ra8_add_zig_library(
    NAME ra8_imgdec
    ZIG_ROOT ${FW_ROOT}/libs/ra8_imgdec
    LIBRARY_NAME ra8_imgdec
  )
endif()
link_libraries(ra8_zig::ra8_imgdec)

# The arena the JOF producer carves its scratch from is Zig (#2601), so it is
# linked as an archive rather than named as a source. ra8_webp_attach() below
# relies on the same archive for the decoder scratch.
if(NOT TARGET ra8_zig::ra8_mem)
  include(${FW_ROOT}/tests/cmake/zig_library.cmake)
  ra8_add_zig_library(
    NAME ra8_mem
    ZIG_ROOT ${FW_ROOT}/libs/ra8_mem
    LIBRARY_NAME ra8_mem
  )
endif()
link_libraries(ra8_zig::ra8_mem)

if(NOT TARGET ra8_zig::ra8_jpeg)
  include(${FW_ROOT}/tests/cmake/zig_library.cmake)
  ra8_add_zig_library(
    NAME ra8_jpeg
    ZIG_ROOT ${FW_ROOT}/libs/ra8_jpeg
    LIBRARY_NAME ra8_jpeg
  )
endif()
link_libraries(ra8_zig::ra8_jpeg)

# ra8_log is Zig now (#2836), so the ra8_core archive replaces the
# libs/ra8_core/src/ra8_log.c row that used to be compiled by path here.
if(NOT TARGET ra8_zig::ra8_core)
  include(${FW_ROOT}/tests/cmake/zig_library.cmake)
  ra8_add_zig_library(
    NAME ra8_core
    ZIG_ROOT ${FW_ROOT}/libs/ra8_core
    LIBRARY_NAME ra8_core_zig
  )
endif()
link_libraries(ra8_zig::ra8_core)

# ---------------------------------------------------------------------------
# The exact firmware members the tool calls: the JOF producer (with its
# JPEG / PNG / WebP front ends) and reader, plus the compression seam they sit
# on. Naming members rather than globbing keeps the HAL and peripheral layers
# out of a host tool.
# ---------------------------------------------------------------------------
set(RA8_FMT_FW_SRC
    ${FW_ROOT}/apps/shared_libs/jof/src/jof.c
    ${FW_ROOT}/apps/shared_libs/jof/src/jof_produce.c
    ${FW_ROOT}/apps/shared_libs/jof/src/jof_produce_webp.c
    ${FW_ROOT}/apps/shared_libs/jof/src/jof_png.c
    ${FW_ROOT}/apps/shared_libs/jof/src/jof_png_chunk.c
    ${FW_ROOT}/apps/shared_libs/compress/src/ra8_compress.c
)
# The JPEG codec is Zig now: the encoder moved in #2798 and the decoder plus
# its stripe driver in #2799, so libs/ra8_jpeg/src holds no .c at all. The
# hand-authored C23 header under libs/ra8_jpeg/inc is unchanged, so the
# targets below that used to name the three decoder sources by path link the
# archive instead. RA8_FMT_JPEG_SRC stays defined and empty: it is still
# expanded in three source lists below, and emptying it here keeps those
# lists readable rather than editing each one.
set(RA8_FMT_JPEG_SRC)

set(RA8_FMT_THIRD_PARTY_SRC ${FW_ROOT}/apps/shared_libs/third_party/miniz/miniz.c)
set(RA8_FMT_MINIZ_DEFINITIONS MINIZ_NO_ARCHIVE_APIS MINIZ_NO_MALLOC MINIZ_NO_STDIO MINIZ_NO_TIME)
set(RA8_FMT_BOOK_SRC
    ${FW_ROOT}/apps/shared_libs/book/src/book.c
    ${FW_ROOT}/apps/shared_libs/book/src/book_chunked.c
    ${FW_ROOT}/apps/shared_libs/book/src/book_chunked_validate.c
    ${FW_ROOT}/apps/shared_libs/book/src/book_stream.c
    ${FW_ROOT}/apps/shared_libs/book/src/book_stream_wire.c
)

add_executable(
  rabook_imagepack
  src/main.c
  src/ra8_fmt_stream_convert.c
  src/ra8_fmt_stream_inspect.c
  src/ra8_fmt_stream_rabook.c
  src/ra8_fmt_stream_verify.c
  src/ra8_fmt_stream_verify_plan.c
  src/ra8_fmt_host_fd.c
  src/ra8_fmt_host_spool.c
  src/ra8_fmt_portable_convert.c
  src/ra8_fmt_portable_verify.c
  src/ra8_fmt_portable_main.c
  ${FW_ROOT}/apps/shared_libs/jof/src/jof_audit.c
  ${RA8_FMT_FW_SRC}
  ${RA8_FMT_JPEG_SRC}
  ${RA8_FMT_BOOK_SRC}
  ${RA8_FMT_THIRD_PARTY_SRC}
)

# WebP decode (vendored libwebp + the ra8_webp facade), so the producer's WebP
# arm above is the real transcode rather than a link-satisfying stub. One call:
# the recipe lives in cmake/ra8_webp_vendor.cmake, shared with the firmware app
# recipe and the host test build.
include(${FW_ROOT}/cmake/ra8_webp_vendor.cmake)
ra8_webp_attach(rabook_imagepack ${FW_ROOT})
target_compile_definitions(
  rabook_imagepack PRIVATE RA8_OFF_TARGET ${RA8_FMT_MINIZ_DEFINITIONS}
                           ${RA8_FMT_HOST_FEATURE_DEFINITIONS}
)
target_include_directories(
  rabook_imagepack
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
          ${FW_ROOT}/libs/ra8_core/inc
          ${FW_ROOT}/libs/ra8_fmt/inc
          ${FW_ROOT}/apps/shared_libs/jof/inc
          ${FW_ROOT}/libs/ra8_jpeg/inc
          ${FW_ROOT}/libs/ra8_jpeg/src
          ${FW_ROOT}/apps/shared_libs/jof/src
          ${FW_ROOT}/libs/ra8_io/inc
          ${FW_ROOT}/apps/shared_libs/compress/inc
          ${FW_ROOT}/libs/ra8_imgdec/inc
          ${FW_ROOT}/apps/shared_libs/book/inc
          ${FW_ROOT}/libs/ra8_mem/inc
          ${FW_ROOT}/apps/shared_libs/third_party/miniz
)
# First-party sources carry the project warning bar (NASA Power of 10
# Rule 10) here exactly as they do in the ARM and host-test builds; only
# vendored SOUP is compiled quietly.
target_compile_options(rabook_imagepack PRIVATE -Wall -Wextra -Werror)

include(CTest)
if(BUILD_TESTING)
  # Deliberately NOT named test_jof_audit: the root host-mode build
  # (-DRA8_HOST_MODE=ON) configures tests/ and this listfile into one project,
  # and tests/cmake/unit_tests.cmake already registers a test_jof_audit target
  # for the same test source against ra8_core_hal. Two targets, one name, is a
  # hard CMP0002 configure error. The two builds are not redundant: that one
  # links the shared off-target object library, this one compiles the audit
  # against the imagepack's own minimal closure (jof + compress + miniz, no
  # HAL), which is what "portable" in the jof_portable_audit test name means.
  add_executable(
    test_jof_audit_portable
    ${FW_ROOT}/apps/shared_libs/jof/tests/src/test_jof_audit.c
    ${FW_ROOT}/apps/shared_libs/jof/src/jof_audit.c
    ${FW_ROOT}/apps/shared_libs/jof/src/jof.c
    ${FW_ROOT}/apps/shared_libs/compress/src/ra8_compress.c
    ${RA8_FMT_THIRD_PARTY_SRC}
  )
  target_compile_definitions(
    test_jof_audit_portable PRIVATE RA8_OFF_TARGET ${RA8_FMT_MINIZ_DEFINITIONS}
  )
  target_compile_options(test_jof_audit_portable PRIVATE -Wall -Wextra -Werror)
  target_include_directories(
    test_jof_audit_portable
    PRIVATE ${FW_ROOT}/tests/support/inc
            ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${FW_ROOT}/libs/ra8_core/inc
            ${FW_ROOT}/libs/ra8_fmt/inc
            ${FW_ROOT}/apps/shared_libs/jof/inc
            ${FW_ROOT}/libs/ra8_io/inc
            ${FW_ROOT}/apps/shared_libs/compress/inc
            ${FW_ROOT}/apps/shared_libs/third_party/miniz
  )
  add_test(NAME jof_portable_audit COMMAND test_jof_audit_portable)

  add_executable(
    test_ra8_fmt_stream_inspect
    tests/src/test_ra8_fmt_stream_inspect.c
    src/ra8_fmt_stream_inspect.c
    ${FW_ROOT}/apps/shared_libs/jof/src/jof_audit.c
    ${FW_ROOT}/apps/shared_libs/jof/src/jof.c
    ${FW_ROOT}/apps/shared_libs/compress/src/ra8_compress.c
    ${RA8_FMT_THIRD_PARTY_SRC}
  )
  target_compile_definitions(
    test_ra8_fmt_stream_inspect PRIVATE RA8_OFF_TARGET ${RA8_FMT_MINIZ_DEFINITIONS}
  )
  target_compile_options(test_ra8_fmt_stream_inspect PRIVATE -Wall -Wextra -Werror)
  target_include_directories(
    test_ra8_fmt_stream_inspect
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${FW_ROOT}/libs/ra8_core/inc
            ${FW_ROOT}/libs/ra8_fmt/inc
            ${FW_ROOT}/apps/shared_libs/jof/inc
            ${FW_ROOT}/libs/ra8_io/inc
            ${FW_ROOT}/apps/shared_libs/compress/inc
            ${FW_ROOT}/apps/shared_libs/third_party/miniz
  )
  add_test(NAME ra8_fmt_stream_inspect COMMAND test_ra8_fmt_stream_inspect)

  add_executable(test_ra8_fmt_host_fd tests/src/test_ra8_fmt_host_fd.c src/ra8_fmt_host_fd.c)
  target_compile_definitions(
    test_ra8_fmt_host_fd PRIVATE RA8_OFF_TARGET ${RA8_FMT_HOST_FEATURE_DEFINITIONS}
  )
  target_compile_options(test_ra8_fmt_host_fd PRIVATE -Wall -Wextra -Werror)
  target_include_directories(
    test_ra8_fmt_host_fd
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${FW_ROOT}/libs/ra8_core/inc
            ${FW_ROOT}/libs/ra8_fmt/inc
            ${FW_ROOT}/apps/shared_libs/jof/inc
  )
  add_test(NAME ra8_fmt_host_fd COMMAND test_ra8_fmt_host_fd)

  add_executable(
    test_ra8_fmt_stream_convert
    tests/src/test_ra8_fmt_stream_convert.c
    src/ra8_fmt_stream_convert.c
    src/ra8_fmt_host_fd.c
    ${RA8_FMT_FW_SRC}
    ${RA8_FMT_JPEG_SRC}
    ${RA8_FMT_THIRD_PARTY_SRC}
  )
  ra8_webp_attach(test_ra8_fmt_stream_convert ${FW_ROOT})
  string(CONCAT RA8_FMT_TEST_PNG_DEF "RA8_FMT_TEST_PNG=\""
                "${FW_ROOT}/tests/fixtures/rabook_fixed_layout/OEBPS/images/page1.png\""
  )
  target_compile_definitions(
    test_ra8_fmt_stream_convert PRIVATE RA8_OFF_TARGET ${RA8_FMT_MINIZ_DEFINITIONS}
                                        ${RA8_FMT_TEST_PNG_DEF} ${RA8_FMT_HOST_FEATURE_DEFINITIONS}
  )
  target_compile_options(test_ra8_fmt_stream_convert PRIVATE -Wall -Wextra -Werror)
  target_include_directories(
    test_ra8_fmt_stream_convert
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${FW_ROOT}/libs/ra8_core/inc
            ${FW_ROOT}/libs/ra8_fmt/inc
            ${FW_ROOT}/apps/shared_libs/jof/inc
            ${FW_ROOT}/libs/ra8_jpeg/inc
            ${FW_ROOT}/libs/ra8_jpeg/src
            ${FW_ROOT}/apps/shared_libs/jof/src
            ${FW_ROOT}/libs/ra8_io/inc
            ${FW_ROOT}/apps/shared_libs/compress/inc
            ${FW_ROOT}/libs/ra8_imgdec/inc
            ${FW_ROOT}/libs/ra8_mem/inc
            ${FW_ROOT}/apps/shared_libs/third_party/miniz
  )
  add_test(NAME ra8_fmt_stream_convert COMMAND test_ra8_fmt_stream_convert)
  add_executable(
    test_ra8_fmt_stream_verify
    tests/src/test_ra8_fmt_stream_verify.c
    src/ra8_fmt_stream_verify.c
    src/ra8_fmt_stream_verify_plan.c
    src/ra8_fmt_stream_convert.c
    ${RA8_FMT_FW_SRC}
    ${RA8_FMT_JPEG_SRC}
    ${RA8_FMT_THIRD_PARTY_SRC}
  )
  ra8_webp_attach(test_ra8_fmt_stream_verify ${FW_ROOT})
  target_compile_definitions(
    test_ra8_fmt_stream_verify PRIVATE RA8_OFF_TARGET ${RA8_FMT_MINIZ_DEFINITIONS}
                                       ${RA8_FMT_TEST_PNG_DEF}
  )
  target_compile_options(test_ra8_fmt_stream_verify PRIVATE -Wall -Wextra -Werror)
  target_include_directories(
    test_ra8_fmt_stream_verify
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${FW_ROOT}/libs/ra8_core/inc
            ${FW_ROOT}/libs/ra8_fmt/inc
            ${FW_ROOT}/apps/shared_libs/jof/inc
            ${FW_ROOT}/libs/ra8_jpeg/inc
            ${FW_ROOT}/libs/ra8_jpeg/src
            ${FW_ROOT}/apps/shared_libs/jof/src
            ${FW_ROOT}/libs/ra8_io/inc
            ${FW_ROOT}/apps/shared_libs/compress/inc
            ${FW_ROOT}/libs/ra8_imgdec/inc
            ${FW_ROOT}/libs/ra8_mem/inc
            ${FW_ROOT}/apps/shared_libs/third_party/miniz
  )
  add_test(NAME ra8_fmt_stream_verify COMMAND test_ra8_fmt_stream_verify)

  add_executable(
    test_ra8_fmt_stream_rabook
    tests/src/test_ra8_fmt_stream_rabook.c
    src/ra8_fmt_stream_rabook.c
    ${FW_ROOT}/apps/shared_libs/rabook_compile/tests/src/rabook_compile_test_fixture.c
    ${FW_ROOT}/apps/shared_libs/rabook_compile/src/rabook_compile.c
    ${FW_ROOT}/apps/shared_libs/rabook_compile/src/ra8_rabook_finalize.c
    ${FW_ROOT}/apps/shared_libs/rabook_compile/src/ra8_rabook_container.c
    ${RA8_FMT_BOOK_SRC}
    ${FW_ROOT}/apps/shared_libs/compress/src/ra8_compress.c
    ${RA8_FMT_THIRD_PARTY_SRC}
  )
  target_compile_definitions(
    test_ra8_fmt_stream_rabook PRIVATE RA8_OFF_TARGET ${RA8_FMT_MINIZ_DEFINITIONS}
  )
  target_compile_options(test_ra8_fmt_stream_rabook PRIVATE -Wall -Wextra -Werror)
  target_include_directories(
    test_ra8_fmt_stream_rabook
    PRIVATE ${FW_ROOT}/tests/support/inc
            ${FW_ROOT}/apps/shared_libs/rabook_compile/tests/inc
            ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${FW_ROOT}/libs/ra8_core/inc
            ${FW_ROOT}/libs/ra8_fmt/inc
            ${FW_ROOT}/apps/shared_libs/book/inc
            ${FW_ROOT}/apps/shared_libs/book/src
            ${FW_ROOT}/libs/ra8_io/inc
            ${FW_ROOT}/apps/shared_libs/compress/inc
            ${FW_ROOT}/apps/shared_libs/jof/inc
            ${FW_ROOT}/libs/ra8_mem/inc
            ${FW_ROOT}/apps/shared_libs/rabook_compile/inc
            ${FW_ROOT}/apps/shared_libs/third_party/miniz
  )
  add_test(NAME ra8_fmt_stream_rabook COMMAND test_ra8_fmt_stream_rabook)
  add_test(NAME ra8_fmt_rabook_inspect_integration
           COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/rabook_inspect_integration.sh
                   $<TARGET_FILE:rabook_imagepack> $<TARGET_FILE:test_ra8_fmt_stream_rabook>
  )

  add_test(NAME ra8_fmt_convert_integration
           COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/convert_integration.sh
                   $<TARGET_FILE:rabook_imagepack> ${FW_ROOT}
  )
  add_test(NAME ra8_fmt_verify_integration
           COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_integration.sh
                   $<TARGET_FILE:rabook_imagepack> ${FW_ROOT}
  )
  add_test(NAME ra8_fmt_production_surface
           COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_production_surface.sh
                   ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:rabook_imagepack>
  )
endif()

# miniz type-puns through byte buffers; disable strict aliasing to prevent the
# corresponding miscompile without demoting unrelated diagnostics.
set_source_files_properties(
  ${RA8_FMT_THIRD_PARTY_SRC} PROPERTIES COMPILE_OPTIONS "-fno-strict-aliasing"
)

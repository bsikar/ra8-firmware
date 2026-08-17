# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# tools/rabook_imagepack -- single-file converter / inspector for the first-party
# content container formats.
#
# The single-unit counterpart to the batch producers (apps/stand_alone/media_dl,
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

# Firmware root (two directories up from tools/rabook_imagepack/).
get_filename_component(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# ---------------------------------------------------------------------------
# The exact firmware members the tool calls: the JOF producer (with its
# JPEG / PNG / WebP front ends) and reader, plus the compression seam they sit
# on. Naming members rather than globbing keeps the HAL and peripheral layers
# out of a host tool.
# ---------------------------------------------------------------------------
set(RA8_FMT_FW_SRC
    ${FW_ROOT}/libs/ra8_jof/src/ra8_jof.c
    ${FW_ROOT}/libs/ra8_jof/src/ra8_jof_produce.c
    ${FW_ROOT}/libs/ra8_jof/src/ra8_jof_produce_webp.c
    ${FW_ROOT}/libs/ra8_jof/src/ra8_jof_png.c
    ${FW_ROOT}/libs/ra8_jof/src/ra8_jof_png_chunk.c
    ${FW_ROOT}/libs/ra8_io/src/ra8_io_compress.c
    ${FW_ROOT}/libs/ra8_core/src/ra8_log.c
    ${FW_ROOT}/libs/ra8_core/src/ra8_error_handler.c
    ${FW_ROOT}/libs/ra8_core/src/ra8_decomp_limits.c
)
set(RA8_FMT_JPEG_SRC
    ${FW_ROOT}/libs/ra8_jpeg/src/ra8_jpeg_sw.c ${FW_ROOT}/libs/ra8_jpeg/src/ra8_jpeg_sw_decode.c
    ${FW_ROOT}/libs/ra8_jpeg/src/ra8_jpeg_sw_stream.c
)

set(RA8_FMT_THIRD_PARTY_SRC ${FW_ROOT}/libs/third_party/miniz/miniz.c)
set(RA8_FMT_MINIZ_DEFINITIONS MINIZ_NO_ARCHIVE_APIS MINIZ_NO_MALLOC MINIZ_NO_STDIO MINIZ_NO_TIME)
set(RA8_FMT_BOOK_SRC
    ${FW_ROOT}/libs/ra8_book/src/ra8_book.c ${FW_ROOT}/libs/ra8_book/src/ra8_book_chunked.c
    ${FW_ROOT}/libs/ra8_book/src/ra8_book_chunked_validate.c
    ${FW_ROOT}/libs/ra8_book/src/ra8_book_stream.c
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
  ${FW_ROOT}/libs/ra8_jof/src/ra8_jof_audit.c
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
target_compile_definitions(rabook_imagepack PRIVATE RA8_OFF_TARGET ${RA8_FMT_MINIZ_DEFINITIONS})
target_include_directories(
  rabook_imagepack
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${FW_ROOT}/libs/ra8_core/inc
          ${FW_ROOT}/libs/ra8_jof/inc
          ${FW_ROOT}/libs/ra8_jpeg/inc
          ${FW_ROOT}/libs/ra8_jpeg/src
          ${FW_ROOT}/libs/ra8_jof/src
          ${FW_ROOT}/libs/ra8_io/inc
          ${FW_ROOT}/libs/ra8_book/inc
          ${FW_ROOT}/libs/ra8_mem/inc
          ${FW_ROOT}/libs/third_party/miniz
)
# First-party sources carry the project warning bar (NASA Power of 10
# Rule 10) here exactly as they do in the ARM and host-test builds; only
# vendored SOUP is compiled quietly.
target_compile_options(rabook_imagepack PRIVATE -Wall -Wextra -Werror)

include(CTest)
if(BUILD_TESTING)
  add_executable(
    test_ra8_jof_audit
    ${FW_ROOT}/tests/test_ra8_jof_audit.c
    ${FW_ROOT}/libs/ra8_jof/src/ra8_jof_audit.c
    ${FW_ROOT}/libs/ra8_jof/src/ra8_jof.c
    ${FW_ROOT}/libs/ra8_io/src/ra8_io_compress.c
    ${FW_ROOT}/libs/ra8_core/src/ra8_log.c
    ${FW_ROOT}/libs/ra8_core/src/ra8_error_handler.c
    ${FW_ROOT}/libs/ra8_core/src/ra8_decomp_limits.c
    ${RA8_FMT_THIRD_PARTY_SRC}
  )
  target_compile_definitions(test_ra8_jof_audit PRIVATE RA8_OFF_TARGET ${RA8_FMT_MINIZ_DEFINITIONS})
  target_compile_options(test_ra8_jof_audit PRIVATE -Wall -Wextra -Werror)
  target_include_directories(
    test_ra8_jof_audit
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${FW_ROOT}/libs/ra8_core/inc
            ${FW_ROOT}/libs/ra8_jof/inc
            ${FW_ROOT}/libs/ra8_io/inc
            ${FW_ROOT}/libs/third_party/miniz
  )
  add_test(NAME ra8_jof_portable_audit COMMAND test_ra8_jof_audit)

  add_executable(
    test_ra8_fmt_stream_inspect
    tests/test_ra8_fmt_stream_inspect.c
    src/ra8_fmt_stream_inspect.c
    ${FW_ROOT}/libs/ra8_jof/src/ra8_jof_audit.c
    ${FW_ROOT}/libs/ra8_jof/src/ra8_jof.c
    ${FW_ROOT}/libs/ra8_io/src/ra8_io_compress.c
    ${FW_ROOT}/libs/ra8_core/src/ra8_log.c
    ${FW_ROOT}/libs/ra8_core/src/ra8_error_handler.c
    ${FW_ROOT}/libs/ra8_core/src/ra8_decomp_limits.c
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
            ${FW_ROOT}/libs/ra8_jof/inc
            ${FW_ROOT}/libs/ra8_io/inc
            ${FW_ROOT}/libs/third_party/miniz
  )
  add_test(NAME ra8_fmt_stream_inspect COMMAND test_ra8_fmt_stream_inspect)

  add_executable(test_ra8_fmt_host_fd tests/test_ra8_fmt_host_fd.c src/ra8_fmt_host_fd.c)
  target_compile_definitions(test_ra8_fmt_host_fd PRIVATE RA8_OFF_TARGET)
  target_compile_options(test_ra8_fmt_host_fd PRIVATE -Wall -Wextra -Werror)
  target_include_directories(
    test_ra8_fmt_host_fd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc ${CMAKE_CURRENT_SOURCE_DIR}/src
                                 ${FW_ROOT}/libs/ra8_core/inc ${FW_ROOT}/libs/ra8_jof/inc
  )
  add_test(NAME ra8_fmt_host_fd COMMAND test_ra8_fmt_host_fd)

  add_executable(
    test_ra8_fmt_stream_convert
    tests/test_ra8_fmt_stream_convert.c
    src/ra8_fmt_stream_convert.c
    src/ra8_fmt_host_fd.c
    ${RA8_FMT_FW_SRC}
    ${RA8_FMT_JPEG_SRC}
    ${RA8_FMT_THIRD_PARTY_SRC}
  )
  ra8_webp_attach(test_ra8_fmt_stream_convert ${FW_ROOT})
  target_compile_definitions(
    test_ra8_fmt_stream_convert
    PRIVATE RA8_OFF_TARGET ${RA8_FMT_MINIZ_DEFINITIONS}
            RA8_FMT_TEST_PNG="${FW_ROOT}/tests/fixtures/rabook_fixed_layout/OEBPS/images/page1.png"
  )
  target_compile_options(test_ra8_fmt_stream_convert PRIVATE -Wall -Wextra -Werror)
  target_include_directories(
    test_ra8_fmt_stream_convert
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${FW_ROOT}/libs/ra8_core/inc
            ${FW_ROOT}/libs/ra8_jof/inc
            ${FW_ROOT}/libs/ra8_jpeg/inc
            ${FW_ROOT}/libs/ra8_jpeg/src
            ${FW_ROOT}/libs/ra8_jof/src
            ${FW_ROOT}/libs/ra8_io/inc
            ${FW_ROOT}/libs/ra8_mem/inc
            ${FW_ROOT}/libs/third_party/miniz
  )
  add_test(NAME ra8_fmt_stream_convert COMMAND test_ra8_fmt_stream_convert)
  add_executable(
    test_ra8_fmt_stream_verify
    tests/test_ra8_fmt_stream_verify.c
    src/ra8_fmt_stream_verify.c
    src/ra8_fmt_stream_verify_plan.c
    src/ra8_fmt_stream_convert.c
    ${RA8_FMT_FW_SRC}
    ${RA8_FMT_JPEG_SRC}
    ${RA8_FMT_THIRD_PARTY_SRC}
  )
  ra8_webp_attach(test_ra8_fmt_stream_verify ${FW_ROOT})
  target_compile_definitions(
    test_ra8_fmt_stream_verify
    PRIVATE RA8_OFF_TARGET ${RA8_FMT_MINIZ_DEFINITIONS}
            RA8_FMT_TEST_PNG="${FW_ROOT}/tests/fixtures/rabook_fixed_layout/OEBPS/images/page1.png"
  )
  target_compile_options(test_ra8_fmt_stream_verify PRIVATE -Wall -Wextra -Werror)
  target_include_directories(
    test_ra8_fmt_stream_verify
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${FW_ROOT}/libs/ra8_core/inc
            ${FW_ROOT}/libs/ra8_jof/inc
            ${FW_ROOT}/libs/ra8_jpeg/inc
            ${FW_ROOT}/libs/ra8_jpeg/src
            ${FW_ROOT}/libs/ra8_jof/src
            ${FW_ROOT}/libs/ra8_io/inc
            ${FW_ROOT}/libs/ra8_mem/inc
            ${FW_ROOT}/libs/third_party/miniz
  )
  add_test(NAME ra8_fmt_stream_verify COMMAND test_ra8_fmt_stream_verify)

  add_executable(
    test_ra8_fmt_stream_rabook
    tests/test_ra8_fmt_stream_rabook.c
    src/ra8_fmt_stream_rabook.c
    ${FW_ROOT}/tests/support/rabook_compile_test_fixture.c
    ${FW_ROOT}/libs/ra8_rabook_compile/src/ra8_rabook_compile.c
    ${FW_ROOT}/libs/ra8_rabook_compile/src/ra8_rabook_finalize.c
    ${FW_ROOT}/libs/ra8_rabook_compile/src/ra8_rabook_container.c
    ${RA8_FMT_BOOK_SRC}
    ${FW_ROOT}/libs/ra8_io/src/ra8_io_compress.c
    ${FW_ROOT}/libs/ra8_core/src/ra8_log.c
    ${FW_ROOT}/libs/ra8_core/src/ra8_error_handler.c
    ${RA8_FMT_THIRD_PARTY_SRC}
  )
  target_compile_definitions(
    test_ra8_fmt_stream_rabook PRIVATE RA8_OFF_TARGET ${RA8_FMT_MINIZ_DEFINITIONS}
  )
  target_compile_options(test_ra8_fmt_stream_rabook PRIVATE -Wall -Wextra -Werror)
  target_include_directories(
    test_ra8_fmt_stream_rabook
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${FW_ROOT}/tests
            ${FW_ROOT}/libs/ra8_core/inc
            ${FW_ROOT}/libs/ra8_book/inc
            ${FW_ROOT}/libs/ra8_book/src
            ${FW_ROOT}/libs/ra8_io/inc
            ${FW_ROOT}/libs/ra8_jof/inc
            ${FW_ROOT}/libs/ra8_mem/inc
            ${FW_ROOT}/libs/ra8_rabook_compile/inc
            ${FW_ROOT}/libs/third_party/miniz
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

# miniz type-puns through byte buffers; match the host-test build's handling.
set_source_files_properties(
  ${RA8_FMT_THIRD_PARTY_SRC} PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing"
)

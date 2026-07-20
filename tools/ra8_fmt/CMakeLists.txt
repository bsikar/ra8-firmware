#
# tools/ra8_fmt -- single-file converter / inspector for the first-party
# content container formats.
#
# The single-unit counterpart to the batch producers (tools/media_dl,
# tools/epub_compile): convert ONE file into ONE container, dump a container's
# structure, or round-trip a source to prove the produced bytes are correct.
#
#   cmake -B build -S . && cmake --build build -j
#   ./build/ra8_fmt convert --format jof --in page.jpg --out page.jof
#   ./build/ra8_fmt inspect page.jof --verbose
#   ./build/ra8_fmt verify  --format jof --in page.jpg --out recon.ppm
#
# It links the firmware's own producer and reader (not a host reimplementation),
# so a defect reproduced here is a defect on silicon and a clean result here
# exonerates the file.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

project(ra8_fmt LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Firmware root (two directories up from tools/ra8_fmt/).
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
    ${FW_ROOT}/libs/ra8_hal/src/ra8_jpeg_sw.c
    ${FW_ROOT}/libs/ra8_hal/src/ra8_jpeg_sw_decode.c
    ${FW_ROOT}/libs/ra8_hal/src/ra8_jpeg_sw_stream.c
)

set(RA8_FMT_THIRD_PARTY_SRC ${FW_ROOT}/libs/third_party/miniz/miniz.c)

add_executable(ra8_fmt
    src/main.c
    src/ra8_fmt_util.c
    src/ra8_fmt_registry.c
    src/ra8_fmt_atlas.c
    src/ra8_fmt_atlas_inspect.c
    src/ra8_fmt_atlas_verify.c
    src/ra8_fmt_book.c
    ${RA8_FMT_FW_SRC}
    ${RA8_FMT_JPEG_SRC}
    ${RA8_FMT_THIRD_PARTY_SRC}
)

# WebP decode (vendored libwebp + the ra8_webp facade), so the producer's WebP
# arm above is the real transcode rather than a link-satisfying stub. One call:
# the recipe lives in cmake/ra8_webp_vendor.cmake, shared with the firmware app
# recipe and the host test build.
include(${FW_ROOT}/cmake/ra8_webp_vendor.cmake)
ra8_webp_attach(ra8_fmt ${FW_ROOT})
target_compile_definitions(ra8_fmt PRIVATE RA8_SIMULATOR_MODE)
target_include_directories(ra8_fmt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FW_ROOT}/libs/ra8_core/inc
    ${FW_ROOT}/libs/ra8_jof/inc
    ${FW_ROOT}/libs/ra8_hal/inc
    ${FW_ROOT}/libs/ra8_hal/src
    ${FW_ROOT}/libs/ra8_jof/src
    ${FW_ROOT}/libs/ra8_io/inc
    ${FW_ROOT}/libs/ra8_book/inc
    ${FW_ROOT}/libs/ra8_mem/inc
    ${FW_ROOT}/libs/third_party/miniz
)
target_compile_options(ra8_fmt PRIVATE -Wall -Wextra)

# miniz type-puns through byte buffers; match the host-test build's handling.
set_source_files_properties(${RA8_FMT_THIRD_PARTY_SRC}
    PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing")

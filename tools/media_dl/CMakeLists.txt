# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
# media_dl -- native host CLI for the RA8 e-reader's comic/manga/manhwa/webtoon
# downloader. Builds for the DEV MACHINE (x86_64 / arm64), NOT the ARM target:
# it links the firmware's platform-agnostic pieces (currently the ra8_err
# contract; ra8_tileatlas / ra8_fs to follow) behind dependency-inversion seams
# and injects a host libcurl backend for the network. The logic above the seam
# is the same code the RA8 will run once the ESP32-C6 radio backend exists.
cmake_minimum_required(VERSION 3.20)
project(media_dl C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)

# Vendored miniz (the same third-party ZIP/DEFLATE the firmware comic reader
# uses) provides CBZ writing and gzip DEFLATE -- so archive export needs no
# system libraries. SOUP: compiled warning-free, exempt from our style bar.
set(MDL_MINIZ ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/third_party/miniz/miniz.c)
set_source_files_properties(${MDL_MINIZ} PROPERTIES
  COMPILE_OPTIONS "-w;-fno-strict-aliasing")

# The JOF exporter reuses the firmware's own tile-atlas producer + streaming
# JPEG decoder host-side (RA8_SIMULATOR_MODE), so a .jof the CLI writes is
# byte-identical to one the RA8 produces. Compiled quietly: the repo lints these
# via its own build, not this tool's scope.
set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../../libs)
set(MDL_JOF_SRC
  ${FW}/ra8_tileatlas/src/ra8_tileatlas.c
  ${FW}/ra8_tileatlas/src/ra8_tileatlas_produce.c
  ${FW}/ra8_tileatlas/src/ra8_tileatlas_produce_webp.c
  ${FW}/ra8_tileatlas/src/ra8_tileatlas_png.c
  ${FW}/ra8_tileatlas/src/ra8_tileatlas_png_chunk.c
  ${FW}/ra8_hal/src/ra8_jpeg_sw.c
  ${FW}/ra8_hal/src/ra8_jpeg_sw_decode.c
  ${FW}/ra8_hal/src/ra8_jpeg_sw_stream.c
  ${FW}/ra8_io/src/ra8_io_compress.c
  ${FW}/ra8_core/src/ra8_log.c
  ${FW}/ra8_core/src/ra8_error_handler.c
  ${FW}/ra8_core/src/ra8_decomp_limits.c)
set_source_files_properties(${MDL_JOF_SRC} PROPERTIES
  COMPILE_OPTIONS "-w;-fno-strict-aliasing")
set(MDL_JOF_INC
  ${FW}/ra8_tileatlas/inc ${FW}/ra8_tileatlas/src
  ${FW}/ra8_hal/inc ${FW}/ra8_hal/src
  ${FW}/ra8_io/inc)

# WebP decode for the JOF exporter: the sites this CLI scrapes serve .webp pages
# (mdl_export's page-extension table has always accepted them), so the producer's
# WebP arm must be the real transcode. One call; the recipe is shared with the
# firmware app build and the host test build.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ra8_webp_vendor.cmake)
# rabook shells out to this python emitter (the RBKC container the C path can't
# write); pass its directory so the CLI can find cbz_compile.py at runtime.
set(MDL_EPUB_COMPILE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../epub_compile)

add_executable(media_dl
  src/main.c
  src/mdl_net_curl.c
  src/mdl_extract.c
  src/mdl_politeness.c
  src/mdl_config.c
  src/mdl_export.c
  ${MDL_JOF_SRC}
  ${MDL_MINIZ})

target_include_directories(media_dl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/inc
  # Reuse the firmware error contract host-side. RA8_SIMULATOR_MODE is the same
  # switch the host unit-test build uses so register-reaching code takes the
  # host-safe path.
  ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
  ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/third_party/miniz
  ${MDL_JOF_INC})

target_compile_definitions(media_dl PRIVATE
  RA8_SIMULATOR_MODE
  MDL_EPUB_COMPILE_DIR="${MDL_EPUB_COMPILE_DIR}")
target_compile_options(media_dl PRIVATE -Wall -Wextra)
target_link_libraries(media_dl PRIVATE CURL::libcurl)
ra8_webp_attach(media_dl ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Host unit tests (unity_minimal harness, like the repo's tests/). The pure
# logic under test (extract/config/export) needs no libcurl, so the test binary
# links only those units + vendored miniz.
enable_testing()
add_executable(test_media_dl
  tests/test_media_dl.c
  src/mdl_extract.c
  src/mdl_config.c
  src/mdl_export.c
  ${MDL_JOF_SRC}
  ${MDL_MINIZ})
target_include_directories(test_media_dl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/inc
  ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
  ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/third_party/miniz
  ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
  ${MDL_JOF_INC})
target_compile_definitions(test_media_dl PRIVATE
  RA8_SIMULATOR_MODE
  MDL_EPUB_COMPILE_DIR="${MDL_EPUB_COMPILE_DIR}")
target_compile_options(test_media_dl PRIVATE -Wall -Wextra)
ra8_webp_attach(test_media_dl ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_test(NAME test_media_dl COMMAND test_media_dl)

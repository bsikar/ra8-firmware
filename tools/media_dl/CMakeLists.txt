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

add_executable(media_dl
  src/main.c
  src/mdl_net_curl.c
  src/mdl_extract.c
  src/mdl_politeness.c
  src/mdl_config.c
  src/mdl_export.c
  ${MDL_MINIZ})

target_include_directories(media_dl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/inc
  # Reuse the firmware error contract host-side. RA8_SIMULATOR_MODE is the same
  # switch the host unit-test build uses so register-reaching code takes the
  # host-safe path.
  ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
  ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/third_party/miniz)

target_compile_definitions(media_dl PRIVATE RA8_SIMULATOR_MODE)
target_compile_options(media_dl PRIVATE -Wall -Wextra)
target_link_libraries(media_dl PRIVATE CURL::libcurl)

# Host unit tests (unity_minimal harness, like the repo's tests/). The pure
# logic under test (extract/config/export) needs no libcurl, so the test binary
# links only those units + vendored miniz.
enable_testing()
add_executable(test_media_dl
  tests/test_media_dl.c
  src/mdl_extract.c
  src/mdl_config.c
  src/mdl_export.c
  ${MDL_MINIZ})
target_include_directories(test_media_dl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/inc
  ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
  ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/third_party/miniz
  ${CMAKE_CURRENT_SOURCE_DIR}/../../tests)
target_compile_definitions(test_media_dl PRIVATE RA8_SIMULATOR_MODE)
target_compile_options(test_media_dl PRIVATE -Wall -Wextra)
add_test(NAME test_media_dl COMMAND test_media_dl)

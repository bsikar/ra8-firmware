# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

cmake_minimum_required(VERSION 3.20)
project(image_pyramid C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(ZIG_EXECUTABLE NAMES zig REQUIRED)

set(IMAGE_PYRAMID_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/zig-out)
set(IMAGE_PYRAMID_BUILT ${IMAGE_PYRAMID_PREFIX}/bin/image_pyramid)
set(IMAGE_PYRAMID_CLI ${CMAKE_CURRENT_BINARY_DIR}/image_pyramid)

add_library(
  image_pyramid_codec_c OBJECT
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/ra8_jpeg/src/ra8_jpeg_sw.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/ra8_jpeg/src/ra8_jpeg_sw_decode.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/ra8_jpeg/src/ra8_jpeg_sw_stream.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/ra8_jpeg/src/ra8_jpeg_sw_encode.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/ra8_jpeg/src/ra8_jpeg_sw_encode_emit.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/ra8_core/src/ra8_log.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/ra8_core/src/ra8_error_handler.c
)
target_compile_definitions(image_pyramid_codec_c PRIVATE RA8_FREESTANDING RA8_OFF_TARGET)
target_compile_options(image_pyramid_codec_c PRIVATE -Wall -Wextra -Werror)
target_include_directories(
  image_pyramid_codec_c
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/ra8_jpeg/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/ra8_jpeg/src
          ${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/ra8_core/inc
)

add_custom_target(
  image_pyramid ALL
  COMMAND ${ZIG_EXECUTABLE} build --prefix ${IMAGE_PYRAMID_PREFIX}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${IMAGE_PYRAMID_BUILT} ${IMAGE_PYRAMID_CLI}
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  BYPRODUCTS ${IMAGE_PYRAMID_BUILT} ${IMAGE_PYRAMID_CLI}
  DEPENDS image_pyramid_codec_c
  VERBATIM
)

enable_testing()
add_test(
  NAME test_image_pyramid
  COMMAND ${ZIG_EXECUTABLE} build test
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

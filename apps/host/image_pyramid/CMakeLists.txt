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

# The JPEG codec, ra8_log and the fatal-error sink are all Zig now, so this
# application has no C source of its own: the codec and its runtime reach the
# Zig application through build.zig, which links those libraries' archives.
add_custom_target(
  image_pyramid ALL
  COMMAND ${ZIG_EXECUTABLE} build --prefix ${IMAGE_PYRAMID_PREFIX}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${IMAGE_PYRAMID_BUILT} ${IMAGE_PYRAMID_CLI}
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  BYPRODUCTS ${IMAGE_PYRAMID_BUILT} ${IMAGE_PYRAMID_CLI}
  COMMENT "Building image_pyramid Zig host application"
  VERBATIM
)

enable_testing()
add_test(
  NAME test_image_pyramid
  COMMAND ${ZIG_EXECUTABLE} build test
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

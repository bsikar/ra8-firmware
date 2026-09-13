# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
cmake_minimum_required(VERSION 3.20)
project(reg_gen NONE)

find_program(ZIG_EXECUTABLE NAMES zig)

if(NOT ZIG_EXECUTABLE)
  message(FATAL_ERROR "reg_gen requires the Zig toolchain (zig not found on PATH)")
endif()

set(REG_GEN_CLI ${CMAKE_CURRENT_BINARY_DIR}/reg_gen)
set(REG_GEN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/main.zig)
set(REG_GEN_JSON ${CMAKE_CURRENT_SOURCE_DIR}/registers.json)
set(REG_GEN_JSON_COPY ${CMAKE_CURRENT_BINARY_DIR}/registers.json)

if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  set(ZIG_TARGET_FLAGS -target aarch64-macos-none)
elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  set(ZIG_TARGET_FLAGS -target x86_64-macos-none)
else()
  set(ZIG_TARGET_FLAGS "")
endif()

add_custom_target(
  reg_gen ALL
  COMMAND ${ZIG_EXECUTABLE} build-exe ${REG_GEN_SRC} ${ZIG_TARGET_FLAGS}
          -femit-bin=${REG_GEN_CLI}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${REG_GEN_JSON}
          ${REG_GEN_JSON_COPY}
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS ${REG_GEN_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/src/generator.zig
          ${REG_GEN_JSON}
  BYPRODUCTS ${REG_GEN_CLI} ${REG_GEN_JSON_COPY}
  COMMENT "Building reg_gen CLI (zig build-exe) and staging registers.json"
  VERBATIM
)

# Unit Tests via CTest
enable_testing()
add_test(
  NAME test_reg_gen
  COMMAND ${ZIG_EXECUTABLE} test ${ZIG_TARGET_FLAGS} ${REG_GEN_SRC}
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

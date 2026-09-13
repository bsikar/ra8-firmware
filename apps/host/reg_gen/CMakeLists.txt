# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
cmake_minimum_required(VERSION 3.20)
project(reg_gen C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(ZIG_EXECUTABLE NAMES zig)

add_executable(
  reg_gen_worker
  src/main.c
  src/reg_gen.c
)

target_include_directories(
  reg_gen_worker PRIVATE inc
)
target_compile_options(reg_gen_worker PRIVATE -Wall -Wextra -Werror)

# Unit Tests
enable_testing()
add_executable(test_reg_gen tests/src/test_reg_gen.c src/reg_gen.c)
target_include_directories(test_reg_gen PRIVATE inc)
target_compile_definitions(test_reg_gen PRIVATE RA8_OFF_TARGET TEST_MODE)
target_compile_options(test_reg_gen PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_reg_gen COMMAND test_reg_gen)

if(ZIG_EXECUTABLE)
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

  add_custom_command(
    OUTPUT ${REG_GEN_CLI}
    COMMAND ${ZIG_EXECUTABLE} build-exe ${REG_GEN_SRC} ${ZIG_TARGET_FLAGS}
            -femit-bin=${REG_GEN_CLI}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS ${REG_GEN_SRC}
    COMMENT "Building reg_gen CLI (zig build-exe)"
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${REG_GEN_JSON_COPY}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${REG_GEN_JSON}
            ${REG_GEN_JSON_COPY}
    DEPENDS ${REG_GEN_JSON}
    COMMENT "Staging registers.json into build directory"
    VERBATIM
  )

  add_custom_target(
    reg_gen_cli ALL
    DEPENDS ${REG_GEN_CLI} ${REG_GEN_JSON_COPY}
    COMMENT "Building reg_gen CLI and staging registers.json"
  )
else()
  message(STATUS "reg_gen: Zig toolchain not found; building C target only")
endif()

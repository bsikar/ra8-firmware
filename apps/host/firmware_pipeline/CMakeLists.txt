# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

cmake_minimum_required(VERSION 3.20)
project(firmware_pipeline C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

find_program(CARGO_EXECUTABLE NAMES cargo REQUIRED)
find_program(ZIG_EXECUTABLE NAMES zig REQUIRED)
find_program(NM_EXECUTABLE NAMES llvm-nm nm REQUIRED)

set(PIPELINE_RUST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rust)
set(PIPELINE_RUST_TARGET ${CMAKE_CURRENT_BINARY_DIR}/cargo-target)
set(PIPELINE_ZIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/zig)
set(PIPELINE_ZIG_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/zig-out)
if(WIN32)
  message(FATAL_ERROR "firmware_pipeline requires a POSIX host; win-ci runs it under WSL")
endif()
set(PIPELINE_RUST_LIBRARY ${PIPELINE_RUST_TARGET}/debug/libfirmware_pipeline_rust.a)
set(PIPELINE_ZIG_LIBRARY ${PIPELINE_ZIG_PREFIX}/lib/libfirmware_pipeline_zig.a)

add_custom_target(
  firmware_pipeline_rust ALL
  COMMAND
    ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${PIPELINE_RUST_TARGET} ${CARGO_EXECUTABLE}
    build --locked --manifest-path ${PIPELINE_RUST_DIR}/Cargo.toml
  BYPRODUCTS ${PIPELINE_RUST_LIBRARY}
  VERBATIM
)
add_custom_target(
  firmware_pipeline_zig ALL
  COMMAND
    ${ZIG_EXECUTABLE} build --build-file ${PIPELINE_ZIG_DIR}/build.zig --prefix
    ${PIPELINE_ZIG_PREFIX} -Drust-lib-dir=${PIPELINE_RUST_TARGET}/debug
  BYPRODUCTS ${PIPELINE_ZIG_LIBRARY}
  DEPENDS firmware_pipeline_rust
  VERBATIM
)

add_executable(
  firmware_pipeline ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/firmware_pipeline_cli.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/firmware_pipeline_io.c
)
target_include_directories(
  firmware_pipeline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(firmware_pipeline PRIVATE -Wall -Wextra -Werror)
target_link_libraries(firmware_pipeline PRIVATE ${PIPELINE_ZIG_LIBRARY} ${PIPELINE_RUST_LIBRARY})
add_dependencies(firmware_pipeline firmware_pipeline_zig)

add_executable(
  test_firmware_pipeline_cli ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.c
                             ${CMAKE_CURRENT_SOURCE_DIR}/src/firmware_pipeline_cli.c
)
target_include_directories(test_firmware_pipeline_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(test_firmware_pipeline_cli PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_firmware_pipeline_cli COMMAND test_firmware_pipeline_cli)

add_executable(test_firmware_pipeline_io ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_io.c
                                         ${CMAKE_CURRENT_SOURCE_DIR}/src/firmware_pipeline_io.c)
target_include_directories(test_firmware_pipeline_io PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(test_firmware_pipeline_io PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_firmware_pipeline_io COMMAND test_firmware_pipeline_io)

add_executable(test_firmware_pipeline_abi ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_abi.c)
target_include_directories(test_firmware_pipeline_abi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_compile_options(test_firmware_pipeline_abi PRIVATE -Wall -Wextra -Werror)
target_link_libraries(
  test_firmware_pipeline_abi PRIVATE ${PIPELINE_ZIG_LIBRARY} ${PIPELINE_RUST_LIBRARY}
)
add_dependencies(test_firmware_pipeline_abi firmware_pipeline_zig)
add_test(NAME test_firmware_pipeline_abi COMMAND test_firmware_pipeline_abi)

add_test(
  NAME test_firmware_pipeline_rust
  COMMAND
    ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${PIPELINE_RUST_TARGET} ${CARGO_EXECUTABLE} test
    --locked --all-features --manifest-path ${PIPELINE_RUST_DIR}/Cargo.toml
)
add_test(
  NAME test_firmware_pipeline_zig
  COMMAND
    ${ZIG_EXECUTABLE} build --build-file ${PIPELINE_ZIG_DIR}/build.zig test
    -Drust-lib-dir=${PIPELINE_RUST_TARGET}/debug
)
add_test(
  NAME test_firmware_pipeline_symbols
  COMMAND
    ${CMAKE_COMMAND} -DNM=${NM_EXECUTABLE} -DZIG_LIBRARY=${PIPELINE_ZIG_LIBRARY}
    -DRUST_LIBRARY=${PIPELINE_RUST_LIBRARY}
    -DPUBLIC_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/inc/firmware_pipeline.h
    -DPRIVATE_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/inc/firmware_pipeline_rust.h
    -DZIG_SOURCE=${PIPELINE_ZIG_DIR}/src/adapter.zig
    -DRUST_SOURCE=${PIPELINE_RUST_DIR}/src/foreign.rs -P
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_symbols.cmake
)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/negative_pipeline.h "missing contract\n")
add_test(
  NAME test_firmware_pipeline_symbols_selftest
  COMMAND
    ${CMAKE_COMMAND} -DNM=${NM_EXECUTABLE} -DZIG_LIBRARY=${PIPELINE_ZIG_LIBRARY}
    -DRUST_LIBRARY=${PIPELINE_RUST_LIBRARY}
    -DPUBLIC_HEADER=${CMAKE_CURRENT_BINARY_DIR}/negative_pipeline.h
    -DPRIVATE_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/inc/firmware_pipeline_rust.h
    -DZIG_SOURCE=${PIPELINE_ZIG_DIR}/src/adapter.zig
    -DRUST_SOURCE=${PIPELINE_RUST_DIR}/src/foreign.rs -P
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_symbols.cmake
)
# This is the must-fire half of the symbol-contract selftest: the deliberately
# incomplete header must be rejected, so a non-zero checker result is success.
set_tests_properties(test_firmware_pipeline_symbols_selftest PROPERTIES WILL_FAIL TRUE)

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/cli_fixture.bin "hello")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/empty_fixture.bin "")
add_test(
  NAME test_firmware_pipeline_command
  COMMAND
    ${CMAKE_COMMAND} -DEXECUTABLE=$<TARGET_FILE:firmware_pipeline>
    -DFIXTURE=${CMAKE_CURRENT_BINARY_DIR}/cli_fixture.bin
    -DEMPTY_FIXTURE=${CMAKE_CURRENT_BINARY_DIR}/empty_fixture.bin -P
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_cli.cmake
)
add_test(
  NAME test_firmware_pipeline_inventory_selftest
  COMMAND
    ${CMAKE_COMMAND} -DACTUAL_TESTS=test_firmware_pipeline_cli -P
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_inventory.cmake
)
# This is the must-fire half of the inventory selftest: an intentionally
# incomplete test-name list must be rejected by the inventory checker.
set_tests_properties(test_firmware_pipeline_inventory_selftest PROPERTIES WILL_FAIL TRUE)

set(
  PIPELINE_REQUIRED_TESTS
  test_firmware_pipeline_abi
  test_firmware_pipeline_cli
  test_firmware_pipeline_command
  test_firmware_pipeline_inventory_selftest
  test_firmware_pipeline_io
  test_firmware_pipeline_rust
  test_firmware_pipeline_symbols
  test_firmware_pipeline_symbols_selftest
  test_firmware_pipeline_zig
)
get_property(PIPELINE_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
list(SORT PIPELINE_REQUIRED_TESTS)
list(SORT PIPELINE_REGISTERED_TESTS)
if(NOT "${PIPELINE_REGISTERED_TESTS}" STREQUAL "${PIPELINE_REQUIRED_TESTS}")
  message(FATAL_ERROR "firmware_pipeline tests ${PIPELINE_REGISTERED_TESTS}; required ${PIPELINE_REQUIRED_TESTS}")
endif()

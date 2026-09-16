# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

cmake_minimum_required(VERSION 3.20)
project(firmware_report C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

find_program(CARGO_EXECUTABLE NAMES cargo REQUIRED)
find_program(NM_EXECUTABLE NAMES llvm-nm nm REQUIRED)

set(FIRMWARE_REPORT_RUST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rust)
set(FIRMWARE_REPORT_TARGET_DIR ${CMAKE_CURRENT_BINARY_DIR}/cargo-target)
if(WIN32)
  set(FIRMWARE_REPORT_RUST_LIBRARY ${FIRMWARE_REPORT_TARGET_DIR}/debug/firmware_report_provider.lib)
else()
  set(FIRMWARE_REPORT_RUST_LIBRARY
      ${FIRMWARE_REPORT_TARGET_DIR}/debug/libfirmware_report_provider.a
  )
endif()
if(WIN32)
  set(FIRMWARE_REPORT_EXECUTABLE ${FIRMWARE_REPORT_TARGET_DIR}/debug/firmware_report.exe)
else()
  set(FIRMWARE_REPORT_EXECUTABLE ${FIRMWARE_REPORT_TARGET_DIR}/debug/firmware_report)
endif()

add_library(firmware_report_c STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/firmware_report_cli.c)
target_include_directories(
  firmware_report_c PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                            ${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/ra8_core/inc
)
target_compile_options(firmware_report_c PRIVATE -Wall -Wextra -Werror)

add_custom_target(
  firmware_report_rust ALL
  COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${FIRMWARE_REPORT_TARGET_DIR} ${CARGO_EXECUTABLE}
          build --locked --lib --manifest-path ${FIRMWARE_REPORT_RUST_DIR}/Cargo.toml
  BYPRODUCTS ${FIRMWARE_REPORT_RUST_LIBRARY}
  COMMENT "Building firmware_report Rust provider"
  VERBATIM
)

add_custom_target(
  firmware_report ALL
  COMMAND
    ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${FIRMWARE_REPORT_TARGET_DIR}
    FIRMWARE_REPORT_C_LIB_DIR=$<TARGET_FILE_DIR:firmware_report_c> ${CARGO_EXECUTABLE} build
    --locked --features
  command --bin firmware_report --manifest-path ${FIRMWARE_REPORT_RUST_DIR}/Cargo.toml
  BYPRODUCTS ${FIRMWARE_REPORT_EXECUTABLE}
  DEPENDS firmware_report_c
  COMMENT "Building Rust firmware_report"
  VERBATIM
)

add_executable(
  test_firmware_report_cli ${CMAKE_CURRENT_SOURCE_DIR}/tests/src/test_cli.c
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/firmware_report_cli.c
)
target_include_directories(
  test_firmware_report_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                                   ${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/ra8_core/inc
)
target_compile_options(test_firmware_report_cli PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_firmware_report_cli COMMAND test_firmware_report_cli)

add_executable(test_firmware_report_abi ${CMAKE_CURRENT_SOURCE_DIR}/tests/src/test_abi.c)
target_include_directories(test_firmware_report_abi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_compile_options(test_firmware_report_abi PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_firmware_report_abi PRIVATE ${FIRMWARE_REPORT_RUST_LIBRARY})
add_dependencies(test_firmware_report_abi firmware_report_rust)
add_test(NAME test_firmware_report_abi COMMAND test_firmware_report_abi)

add_test(
  NAME test_firmware_report_rust
  COMMAND
    ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${FIRMWARE_REPORT_TARGET_DIR} ${CARGO_EXECUTABLE} test
    --locked --all-features --manifest-path ${FIRMWARE_REPORT_RUST_DIR}/Cargo.toml
)
add_test(
  NAME test_firmware_report_symbols
  COMMAND
    ${CMAKE_COMMAND} -DNM=${NM_EXECUTABLE} -DLIBRARY=${FIRMWARE_REPORT_RUST_LIBRARY}
    -DHEADER=${CMAKE_CURRENT_SOURCE_DIR}/inc/firmware_report.h
    -DRUST_SOURCE=${FIRMWARE_REPORT_RUST_DIR}/src/foreign.rs -P
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_symbols.cmake
)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/negative_abi_header.h
     "firmware_report_status_t firmware_report_create(void);\n"
     "firmware_report_status_t firmware_report_release(void);\n"
)
add_test(
  NAME test_firmware_report_symbols_selftest
  COMMAND
    ${CMAKE_COMMAND} -DNM=${NM_EXECUTABLE} -DLIBRARY=${FIRMWARE_REPORT_RUST_LIBRARY}
    -DHEADER=${CMAKE_CURRENT_BINARY_DIR}/negative_abi_header.h
    -DRUST_SOURCE=${FIRMWARE_REPORT_RUST_DIR}/src/foreign.rs -P
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_symbols.cmake
)
# This is the must-fire half of the symbol-contract selftest: the deliberately
# incomplete header must be rejected, so a non-zero checker result is success.
set_tests_properties(test_firmware_report_symbols_selftest PROPERTIES WILL_FAIL TRUE)

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/cli_fixture.bin "hello")
add_test(
  NAME test_firmware_report_command
  COMMAND
    ${CMAKE_COMMAND} -DEXECUTABLE=${FIRMWARE_REPORT_EXECUTABLE}
    -DFIXTURE=${CMAKE_CURRENT_BINARY_DIR}/cli_fixture.bin -P
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_cli.cmake
)

add_test(NAME test_firmware_report_inventory_selftest
         COMMAND ${CMAKE_COMMAND} -DACTUAL_TESTS=test_firmware_report_cli -P
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_test_inventory.cmake
)
# This is the must-fire half of the inventory selftest: an intentionally
# incomplete test-name list must be rejected by the inventory checker.
set_tests_properties(test_firmware_report_inventory_selftest PROPERTIES WILL_FAIL TRUE)

set(FIRMWARE_REPORT_REQUIRED_TESTS
    test_firmware_report_abi
    test_firmware_report_cli
    test_firmware_report_command
    test_firmware_report_inventory_selftest
    test_firmware_report_rust
    test_firmware_report_symbols
    test_firmware_report_symbols_selftest
)
get_property(
  FIRMWARE_REPORT_REGISTERED_TESTS
  DIRECTORY
  PROPERTY TESTS
)
list(SORT FIRMWARE_REPORT_REQUIRED_TESTS)
list(SORT FIRMWARE_REPORT_REGISTERED_TESTS)
if(NOT "${FIRMWARE_REPORT_REGISTERED_TESTS}" STREQUAL "${FIRMWARE_REPORT_REQUIRED_TESTS}")
  message(FATAL_ERROR "firmware_report CTest inventory ${FIRMWARE_REPORT_REGISTERED_TESTS}; "
                      "required ${FIRMWARE_REPORT_REQUIRED_TESTS}"
  )
endif()

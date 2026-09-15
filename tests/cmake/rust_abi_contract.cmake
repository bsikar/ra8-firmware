# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Build a Rust static-library provider and run its public C23 consumer.

find_program(CARGO_EXECUTABLE NAMES cargo REQUIRED)
set(_ra8_rust_abi_root "${CMAKE_CURRENT_SOURCE_DIR}/rust_abi_fixture")
set(_ra8_rust_abi_target "${CMAKE_CURRENT_BINARY_DIR}/rust_abi/target")
set(_ra8_rust_abi_library "${_ra8_rust_abi_target}/debug/libra8_rust_abi_fixture.a")

add_custom_target(
  ra8_rust_abi_fixture_library ALL
  COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${_ra8_rust_abi_target}" "${CARGO_EXECUTABLE}"
          build --locked --manifest-path "${_ra8_rust_abi_root}/Cargo.toml"
  BYPRODUCTS "${_ra8_rust_abi_library}"
  COMMENT "Building Rust C ABI provider fixture"
  VERBATIM
)

add_executable(ra8_rust_abi_fixture_c_consumer "${_ra8_rust_abi_root}/test_c_consumer.c")
target_compile_options(ra8_rust_abi_fixture_c_consumer PRIVATE -Wall -Wextra -Werror)
target_include_directories(
  ra8_rust_abi_fixture_c_consumer PRIVATE "${_ra8_rust_abi_root}/inc"
                                          "${FW_ROOT}/libs/ra8_core/inc"
)
target_link_libraries(ra8_rust_abi_fixture_c_consumer PRIVATE "${_ra8_rust_abi_library}")
add_dependencies(ra8_rust_abi_fixture_c_consumer ra8_rust_abi_fixture_library)
add_test(NAME ra8_rust_abi_fixture_c_consumer COMMAND ra8_rust_abi_fixture_c_consumer)

add_test(
  NAME ra8_rust_abi_fixture_native
  COMMAND
    "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${_ra8_rust_abi_target}" "${CARGO_EXECUTABLE}" test
    --locked --all-features --manifest-path "${_ra8_rust_abi_root}/Cargo.toml"
)

find_program(ZIG_EXECUTABLE NAMES zig REQUIRED)
add_custom_target(
  ra8_rust_abi_fixture_zig_consumer ALL
  COMMAND "${ZIG_EXECUTABLE}" build test "-Drust-lib-dir=${_ra8_rust_abi_target}/debug" --summary
          all
  WORKING_DIRECTORY "${_ra8_rust_abi_root}/zig"
  DEPENDS ra8_rust_abi_fixture_library
  COMMENT "Building Zig consumer of Rust C ABI fixture"
  VERBATIM
)
add_test(
  NAME ra8_rust_abi_fixture_zig_consumer
  COMMAND "${ZIG_EXECUTABLE}" build test "-Drust-lib-dir=${_ra8_rust_abi_target}/debug" --summary
          all
  WORKING_DIRECTORY "${_ra8_rust_abi_root}/zig"
)

find_program(NM_EXECUTABLE NAMES llvm-nm nm REQUIRED)
add_test(
  NAME ra8_rust_abi_fixture_symbols
  COMMAND
    "${CMAKE_COMMAND}" "-DRA8_NM=${NM_EXECUTABLE}" "-DRA8_LIBRARY=${_ra8_rust_abi_library}"
    "-DRA8_HEADER=${_ra8_rust_abi_root}/inc/ra8_rust_abi_fixture.h"
    "-DRA8_RUST_SOURCE=${_ra8_rust_abi_root}/src/foreign.rs" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_rust_abi_symbols.cmake"
)

set(_ra8_chain_root "${CMAKE_CURRENT_SOURCE_DIR}/abi_chain_fixture")
set(_ra8_chain_output "${CMAKE_CURRENT_BINARY_DIR}/abi_chain")
set(_ra8_chain_library "${_ra8_chain_output}/lib/libra8_abi_chain.a")
add_custom_target(
  ra8_abi_chain_library ALL
  COMMAND "${ZIG_EXECUTABLE}" build --prefix "${_ra8_chain_output}" --cache-dir
          "${_ra8_chain_output}/cache" --global-cache-dir "${_ra8_chain_output}/global-cache"
  WORKING_DIRECTORY "${_ra8_chain_root}"
  BYPRODUCTS "${_ra8_chain_library}"
  COMMENT "Building chained Zig ABI adapter"
  VERBATIM
)
add_executable(ra8_abi_chain_c_consumer "${_ra8_chain_root}/test_c_consumer.c")
target_compile_options(ra8_abi_chain_c_consumer PRIVATE -Wall -Wextra -Werror)
target_include_directories(
  ra8_abi_chain_c_consumer PRIVATE "${_ra8_chain_root}/inc" "${_ra8_rust_abi_root}/inc"
                                   "${FW_ROOT}/libs/ra8_core/inc"
)
target_link_libraries(
  ra8_abi_chain_c_consumer PRIVATE "${_ra8_chain_library}" "${_ra8_rust_abi_library}"
)
add_dependencies(ra8_abi_chain_c_consumer ra8_abi_chain_library ra8_rust_abi_fixture_library)
add_test(NAME ra8_abi_chain_c_consumer COMMAND ra8_abi_chain_c_consumer)

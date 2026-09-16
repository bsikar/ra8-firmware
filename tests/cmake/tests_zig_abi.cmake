# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# First consumer of the reusable Zig-to-C ABI contract helper. It is host-only
# and deliberately small: production-library migration remains a later epic.

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/zig_abi_contract.cmake)

ra8_add_zig_c_abi_contract(
  NAME
  ra8_abi_fixture
  ZIG_ROOT
  ${CMAKE_CURRENT_SOURCE_DIR}/zig_abi_fixture
  PUBLIC_INCLUDE_DIR
  ${CMAKE_CURRENT_SOURCE_DIR}/zig_abi_fixture/inc
  C_FIXTURE
  ${CMAKE_CURRENT_SOURCE_DIR}/zig_abi_fixture/src/test_abi_fixture.c
  RUST_CRATE
  ${CMAKE_CURRENT_SOURCE_DIR}/zig_abi_fixture/rust
  LIBRARY_NAME
  ra8_abi_fixture
)

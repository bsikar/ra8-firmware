# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# ra8_c6link host tests: the facade over the ported esp-hosted host
# driver, driven against a co-processor model that speaks the real wire format.
#
# These cannot come from the ra8_add_test() auto-glob. ra8_c6link encodes and
# decodes the vendored esp-hosted `Rpc` protobuf message, so it needs the
# generated codec (esp_hosted_rpc.pb-c.c) and the protobuf-c runtime compiled
# alongside it plus four esp-hosted include directories -- none of which
# ra8_core_hal carries, and none of which the rest of the host build wants.
# Same shape as the LevelX-backed cache-store tests in tests_storage.cmake.
#
# ra8_core_hal is still linked in: it supplies ra8_err / ra8_log / ra8_check.
#
# Included from tests/CMakeLists.txt. CMake include() is textual within the
# same directory scope, so every variable and target defined here is visible
# to the driver and to the fragments included after it.

set(RA8_C6LINK_VENDOR_DIR ${FW_ROOT}/libs/third_party/esp-hosted)

# The vendored subset ra8_c6link reaches: the generated codec and the runtime
# that walks its descriptors. Nothing else from the vendor tree is linked.
set(RA8_C6LINK_SOUP ${RA8_C6LINK_VENDOR_DIR}/common/proto/esp_hosted_rpc.pb-c.c
                    ${RA8_C6LINK_VENDOR_DIR}/common/protobuf-c/protobuf-c/protobuf-c.c
)

set(RA8_C6LINK_INCLUDE_DIRS
    ${FW_ROOT}/libs/ra8_c6link/inc
    ${FW_ROOT}/libs/ra8_c6link/src
    ${FW_ROOT}/libs/ra8_core/inc
    ${FW_ROOT}/port/esp-hosted/inc
    # tests/ itself, for unity_minimal.h -- the mock asserts inside the
    # transport rows, so it needs the harness the tests use.
    ${RA8_TEST_SHARED_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/support/inc
    ${RA8_C6LINK_VENDOR_DIR}/common
    ${RA8_C6LINK_VENDOR_DIR}/common/transport
    ${RA8_C6LINK_VENDOR_DIR}/common/proto
    ${RA8_C6LINK_VENDOR_DIR}/common/protobuf-c
)

file(GLOB RA8_C6LINK_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_c6link/src/*.c)

# The facade suites share one bounded model fixture.
# It owns the decode arena and observation log inside each executable, while its
# internal header exposes only deliberate test operations and observations.
set(RA8_C6LINK_TEST_SUPPORT ${CMAKE_CURRENT_SOURCE_DIR}/support/src/ra8_c6link_model_test.c)
set(RA8_C6LINK_TEST_MODEL ${CMAKE_CURRENT_SOURCE_DIR}/mocks/src/ra8_c6_model.c)

# test_ra8_c6link_wire: the pure layers (decode arena, payload header, TLV
# envelope) on their own, so a failure in the facade test is unambiguous about
# which layer broke. No model, no transport.
add_executable(
  test_ra8_c6link_wire ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_c6link_wire.c
                       ${RA8_C6LINK_SOURCES} ${RA8_C6LINK_SOUP} $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_c6link_wire PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_ra8_c6link_wire PRIVATE -Wall -Wextra)
target_include_directories(test_ra8_c6link_wire PRIVATE ${RA8_C6LINK_INCLUDE_DIRS})
add_test(NAME test_ra8_c6link_wire COMMAND test_ra8_c6link_wire)

# test_ra8_c6link: the whole facade against tests/mocks/src/ra8_c6_model.c, which
# decodes what the host transmits with the same generated codec the ESP32-C6
# runs and synthesises the answer the co-processor would send.
add_executable(
  test_ra8_c6link
  ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_c6link.c
  ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_c6link_session.c
  ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_c6link_transport.c
  ${RA8_C6LINK_TEST_SUPPORT}
  ${RA8_C6LINK_TEST_MODEL}
  ${RA8_C6LINK_SOURCES}
  ${RA8_C6LINK_SOUP}
  $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_c6link PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_ra8_c6link PRIVATE -Wall -Wextra)
target_include_directories(
  test_ra8_c6link PRIVATE ${RA8_C6LINK_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/wireless/inc
)
add_test(NAME test_ra8_c6link COMMAND test_ra8_c6link)

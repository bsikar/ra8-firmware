# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# ra8_c6link host tests (#490): the facade over the ported esp-hosted host
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
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    ${CMAKE_CURRENT_SOURCE_DIR}/support
    ${RA8_C6LINK_VENDOR_DIR}/common
    ${RA8_C6LINK_VENDOR_DIR}/common/transport
    ${RA8_C6LINK_VENDOR_DIR}/common/proto
    ${RA8_C6LINK_VENDOR_DIR}/common/protobuf-c
)

file(GLOB RA8_C6LINK_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_c6link/src/*.c)

# The generic facade and media-transfer suites share one bounded model fixture.
# It owns the decode arena and observation log inside each executable, while its
# internal header exposes only deliberate test operations and observations.
set(RA8_C6LINK_TEST_SUPPORT ${CMAKE_CURRENT_SOURCE_DIR}/support/ra8_c6link_model_test.c)
set(RA8_C6LINK_TEST_MODEL ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra8_c6_model.c
                          ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra8_c6_model_mdl_fault.c
)

# esp-hosted SOUP: silence upstream's warnings, exactly as cmake/esp_hosted.cmake
# does for the cross build. It is already outside the coverage filter
# (libs/third_party/), so instrumentation is unaffected -- the -w only removes
# warnings, not the --coverage flags add_compile_options() applied globally.
set_source_files_properties(${RA8_C6LINK_SOUP} PROPERTIES COMPILE_OPTIONS "-w")

# test_ra8_c6link_wire: the pure layers (decode arena, payload header, TLV
# envelope) on their own, so a failure in the facade test is unambiguous about
# which layer broke. No model, no transport.
add_executable(
  test_ra8_c6link_wire ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_c6link_wire.c ${RA8_C6LINK_SOURCES}
                       ${RA8_C6LINK_SOUP} $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_c6link_wire PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_ra8_c6link_wire PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_include_directories(test_ra8_c6link_wire PRIVATE ${RA8_C6LINK_INCLUDE_DIRS})
add_test(NAME test_ra8_c6link_wire COMMAND test_ra8_c6link_wire)

# test_ra8_c6link: the whole facade against tests/mocks/ra8_c6_model.c, which
# decodes what the host transmits with the same generated codec the ESP32-C6
# runs and synthesises the answer the co-processor would send.
add_executable(
  test_ra8_c6link
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_c6link.c
  ${RA8_C6LINK_TEST_SUPPORT}
  ${RA8_C6LINK_TEST_MODEL}
  ${RA8_C6LINK_SOURCES}
  ${RA8_C6LINK_SOUP}
  $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_c6link PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_ra8_c6link PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_include_directories(test_ra8_c6link PRIVATE ${RA8_C6LINK_INCLUDE_DIRS})
add_test(NAME test_ra8_c6link COMMAND test_ra8_c6link)

# test_ra8_c6link_media: downloader RPC semantics and the transactional
# transfer coordinator, kept independent from the generic Wi-Fi/Ethernet
# facade vectors so each hand-authored translation unit stays below the
# repository's 1000-line cap.
add_executable(
  test_ra8_c6link_media
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_c6link_media.c
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_c6link_media_decoder.c
  ${CMAKE_CURRENT_SOURCE_DIR}/ra8_c6link_transfer_validation_test.c
  ${RA8_C6LINK_TEST_SUPPORT}
  ${RA8_C6LINK_TEST_MODEL}
  ${RA8_C6LINK_SOURCES}
  ${RA8_C6LINK_SOUP}
  $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_c6link_media PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_ra8_c6link_media PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_include_directories(test_ra8_c6link_media PRIVATE ${RA8_C6LINK_INCLUDE_DIRS})
add_test(NAME test_ra8_c6link_media COMMAND test_ra8_c6link_media)

# test_ra8_c6link_mdl: generated inner-protobuf round trips and the portable
# service state machine. It deliberately stays independent of the large host
# HAL object library, so this protocol test also runs on non-Linux hosts.
# Guard: unit_tests.cmake should exclude it from the auto-glob, but cmake
# caching may cause a stale duplicate; if(...NOT TARGET) is defensive.
if(NOT TARGET test_ra8_c6link_mdl)
  add_executable(
    test_ra8_c6link_mdl
    ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_c6link_mdl.c
    ${FW_ROOT}/libs/ra8_c6link/src/ra8_c6link_mdl_service.c
    ${FW_ROOT}/libs/ra8_c6link/src/ra8_media_download.pb-c.c
    ${RA8_C6LINK_VENDOR_DIR}/common/protobuf-c/protobuf-c/protobuf-c.c
  )
  target_compile_options(test_ra8_c6link_mdl PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_include_directories(test_ra8_c6link_mdl PRIVATE ${RA8_C6LINK_INCLUDE_DIRS})
  add_test(NAME test_ra8_c6link_mdl COMMAND test_ra8_c6link_mdl)
endif()

# test_ra8_esp32_c6_mdl_service: the concrete ESP-IDF HTTP adapter compiled
# against deterministic host implementations of its exact consumed SDK
# surface. This reaches the real retained-client/open/read/hash state machine
# through the public CustomRpc hook without requiring a network or C6 board.
add_executable(
  test_ra8_esp32_c6_mdl_service
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_esp32_c6_mdl_service.c
  ${FW_ROOT}/port/esp32_c6/src/mdl_service.c
  ${FW_ROOT}/libs/ra8_c6link/src/ra8_c6link_mdl_service.c
  ${FW_ROOT}/libs/ra8_c6link/src/ra8_media_download.pb-c.c
  ${RA8_C6LINK_VENDOR_DIR}/common/protobuf-c/protobuf-c/protobuf-c.c
)
target_compile_options(
  test_ra8_esp32_c6_mdl_service PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter
)
target_include_directories(
  test_ra8_esp32_c6_mdl_service
  PRIVATE ${RA8_C6LINK_INCLUDE_DIRS} ${FW_ROOT}/port/esp32_c6/inc ${FW_ROOT}/port/esp32_c6/src
          ${FW_ROOT}/port/esp-hosted/inc/idf_compat
)
add_test(NAME test_ra8_esp32_c6_mdl_service COMMAND test_ra8_esp32_c6_mdl_service)

# test_ra8_mdl_storage_vfs: the production media-download transaction binding
# over a real RAM blockdev -> ra8_fs -> named-VFS stack. The adapter depends on
# the c6link-owned storage seam but not the protobuf runtime or transport.
add_executable(
  test_ra8_mdl_storage_vfs
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_mdl_storage_vfs.c
  ${FW_ROOT}/libs/ra8_mdl_storage_vfs/src/ra8_mdl_storage_vfs.c $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_mdl_storage_vfs PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_ra8_mdl_storage_vfs PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_include_directories(
  test_ra8_mdl_storage_vfs
  PRIVATE ${RA8_C6LINK_INCLUDE_DIRS} ${FW_ROOT}/libs/ra8_mdl_storage_vfs/inc
          ${FW_ROOT}/libs/ra8_io/inc ${FW_ROOT}/libs/ra8_fs/inc
)
add_test(NAME test_ra8_mdl_storage_vfs COMMAND test_ra8_mdl_storage_vfs)

# test_ra8_c6link_rabook: the full mixed-image acceptance path. A generated
# RBKC artifact crosses the portable C6 service and RA client, is written to a
# real FAT/VFS transaction, strictly validated before publication, then reopened
# through the same bounded reader workspace for consumption. The C6 model
# decodes and answers the generated inner/outer protobuf messages; it is not a
# replay or a direct call around the transport.
add_executable(
  test_ra8_c6link_rabook
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_c6link_rabook.c
  ${RA8_C6LINK_TEST_SUPPORT}
  ${CMAKE_CURRENT_SOURCE_DIR}/support/rabook_compile_test_fixture.c
  ${RA8_C6LINK_TEST_MODEL}
  ${FW_ROOT}/libs/ra8_mdl_storage_vfs/src/ra8_mdl_storage_vfs.c
  ${FW_ROOT}/libs/ra8_mdl_storage_vfs/src/ra8_mdl_rabook_vfs.c
  ${RA8_C6LINK_SOURCES}
  ${RA8_C6LINK_SOUP}
  $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_c6link_rabook PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_ra8_c6link_rabook PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_include_directories(
  test_ra8_c6link_rabook
  PRIVATE ${RA8_C6LINK_INCLUDE_DIRS}
          ${FW_ROOT}/libs/ra8_mdl_storage_vfs/inc
          ${FW_ROOT}/libs/ra8_book/inc
          ${FW_ROOT}/libs/ra8_rabook_compile/inc
          ${FW_ROOT}/libs/ra8_fs/inc
          ${FW_ROOT}/libs/ra8_hal/inc
          ${FW_ROOT}/libs/ra8_io/inc
          ${FW_ROOT}/libs/ra8_mem/inc
          ${FW_ROOT}/libs/third_party/miniz
)
add_test(NAME test_ra8_c6link_rabook COMMAND test_ra8_c6link_rabook)

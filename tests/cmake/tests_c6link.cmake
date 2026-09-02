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
    ${FW_ROOT}/apps/shared_libs/mdl/tests/inc
    ${FW_ROOT}/apps/shared_libs/mdl/inc
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

# The generic facade and media-transfer suites share one bounded model fixture.
# It owns the decode arena and observation log inside each executable, while its
# internal header exposes only deliberate test operations and observations.
set(RA8_C6LINK_TEST_SUPPORT ${CMAKE_CURRENT_SOURCE_DIR}/support/src/ra8_c6link_model_test.c)
set(RA8_C6LINK_TEST_MODEL ${CMAKE_CURRENT_SOURCE_DIR}/mocks/src/ra8_c6_model.c
                          ${FW_ROOT}/apps/shared_libs/mdl/tests/src/ra8_c6_model_mdl_fault.c
)

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

# test_ra8_c6link_media: downloader RPC semantics and the transactional
# transfer coordinator, kept independent from the generic Wi-Fi/Ethernet
# facade vectors so each hand-authored translation unit stays below the
# repository's 1000-line cap.
add_executable(
  test_ra8_c6link_media
  ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_c6link_media.c
  ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_c6link_media_decoder.c
  ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_c6link_media_http.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/src/ra8_c6link_transfer_validation_test.c
  ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_c6link_transfer_coordinator.c
  ${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_ra8_c6link_mdl_decode.c
  ${RA8_C6LINK_TEST_SUPPORT}
  ${RA8_C6LINK_TEST_MODEL}
  ${RA8_C6LINK_SOURCES}
  ${RA8_C6LINK_SOUP}
  $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_c6link_media PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_ra8_c6link_media PRIVATE -Wall -Wextra)
target_include_directories(
  test_ra8_c6link_media PRIVATE ${RA8_C6LINK_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/wireless/inc
)
add_test(NAME test_ra8_c6link_media COMMAND test_ra8_c6link_media)

# test_mdl_net_c6link_mock: the downloader's real network vtable over the same
# generated protobuf/model transport, with independent SHA verification and
# both caller-buffer and streaming-sink output contracts.
add_executable(
  test_mdl_net_c6link_mock
  ${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_mdl_net_c6link_mock.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../apps/shared_libs/mdl/src/mdl_net.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../apps/shared_libs/mdl/src/mdl_net_c6link.c
  ${RA8_C6LINK_TEST_SUPPORT}
  ${RA8_C6LINK_TEST_MODEL}
  ${RA8_C6LINK_SOURCES}
  ${RA8_C6LINK_SOUP}
  $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_mdl_net_c6link_mock PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_mdl_net_c6link_mock PRIVATE -Wall -Wextra -Werror)
target_include_directories(
  test_mdl_net_c6link_mock PRIVATE ${RA8_C6LINK_INCLUDE_DIRS} ${FW_ROOT}/apps/shared_libs/mdl/inc
                                   ${FW_ROOT}/libs/ra8_hal/inc
)
add_test(NAME test_mdl_net_c6link_mock COMMAND test_mdl_net_c6link_mock)

# test_ra8_c6link_mdl: generated inner-protobuf round trips and the portable
# service state machine. It deliberately stays independent of the large host
# HAL object library, so this protocol test also runs on non-Linux hosts.
# Guard: unit_tests.cmake should exclude it from the auto-glob, but cmake
# caching may cause a stale duplicate; if(...NOT TARGET) is defensive.
if(NOT TARGET test_ra8_c6link_mdl)
  add_executable(
    test_ra8_c6link_mdl
    ${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_ra8_c6link_mdl.c
    ${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_ra8_c6link_mdl_policy.c
    ${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_ra8_c6link_mdl_guards.c
    ${FW_ROOT}/libs/ra8_c6link/src/ra8_c6link_mdl_service.c
    ${FW_ROOT}/libs/ra8_c6link/src/ra8_media_download.pb-c.c
    ${RA8_C6LINK_VENDOR_DIR}/common/protobuf-c/protobuf-c/protobuf-c.c
  )
  target_compile_options(test_ra8_c6link_mdl PRIVATE -Wall -Wextra)
  target_include_directories(test_ra8_c6link_mdl PRIVATE ${RA8_C6LINK_INCLUDE_DIRS})
  add_test(NAME test_ra8_c6link_mdl COMMAND test_ra8_c6link_mdl)
endif()

# test_ra8_c6link_mdl_codec: wire round trips for the generated media-download
# codec itself -- __pack() against __pack_to_buffer() through a
# ProtobufCBufferSimple, __get_packed_size(), field-by-field __unpack(), and
# __free_unpacked(). It is a sibling of test_ra8_c6link_mdl rather than more
# cases inside it because that file is already at the 1000-line cap, and it
# needs strictly less: the generated codec and the protobuf-c runtime, with no
# service state machine and no co-processor model.
if(NOT TARGET test_ra8_c6link_mdl_codec)
  add_executable(
    test_ra8_c6link_mdl_codec
    ${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_ra8_c6link_mdl_codec.c
    ${FW_ROOT}/libs/ra8_c6link/src/ra8_media_download.pb-c.c
    ${RA8_C6LINK_VENDOR_DIR}/common/protobuf-c/protobuf-c/protobuf-c.c
  )
  target_compile_options(test_ra8_c6link_mdl_codec PRIVATE -Wall -Wextra)
  target_include_directories(test_ra8_c6link_mdl_codec PRIVATE ${RA8_C6LINK_INCLUDE_DIRS})
  add_test(NAME test_ra8_c6link_mdl_codec COMMAND test_ra8_c6link_mdl_codec)
endif()

# test_ra8_esp32_c6_mdl_service: the concrete ESP-IDF HTTP adapter compiled
# against deterministic host implementations of its exact consumed SDK
# surface. This reaches the real retained-client/open/read/hash state machine
# through the public CustomRpc hook without requiring a network or C6 board.
add_executable(
  test_ra8_esp32_c6_mdl_service
  ${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_ra8_esp32_c6_mdl_service.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/src/esp32_c6_http_model.c
  ${FW_ROOT}/port/esp32_c6/src/mdl_service.c
  ${FW_ROOT}/libs/ra8_c6link/src/ra8_c6link_mdl_service.c
  ${FW_ROOT}/libs/ra8_c6link/src/ra8_media_download.pb-c.c
  ${RA8_C6LINK_VENDOR_DIR}/common/protobuf-c/protobuf-c/protobuf-c.c
)
target_compile_options(test_ra8_esp32_c6_mdl_service PRIVATE -Wall -Wextra -Werror)
target_include_directories(
  test_ra8_esp32_c6_mdl_service
  PRIVATE ${RA8_C6LINK_INCLUDE_DIRS} ${FW_ROOT}/port/esp32_c6/inc ${FW_ROOT}/port/esp32_c6/src
          ${FW_ROOT}/port/esp-hosted/inc/idf_compat
)
add_test(NAME test_ra8_esp32_c6_mdl_service COMMAND test_ra8_esp32_c6_mdl_service)

# test_mdl_storage_vfs: the production media-download transaction binding
# over a real RAM blockdev -> ra8_fs -> named-VFS stack. The adapter depends on
# the c6link-owned storage seam but not the protobuf runtime or transport.
add_executable(
  test_mdl_storage_vfs
  ${FW_ROOT}/apps/shared_libs/mdl_storage_vfs/tests/src/test_mdl_storage_vfs.c
  ${FW_ROOT}/apps/shared_libs/mdl_storage_vfs/src/mdl_storage_vfs.c $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_mdl_storage_vfs PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_mdl_storage_vfs PRIVATE -Wall -Wextra -Werror)
target_include_directories(
  test_mdl_storage_vfs
  PRIVATE ${RA8_C6LINK_INCLUDE_DIRS}
          ${FW_ROOT}/apps/shared_libs/mdl_storage_vfs/inc
          ${FW_ROOT}/libs/ra8_io/inc
          ${FW_ROOT}/libs/ra8_fs/inc
          ${FW_ROOT}/apps/shared_libs/compress/inc
)
add_test(NAME test_mdl_storage_vfs COMMAND test_mdl_storage_vfs)

# test_mdl_storage_ram: the raw-source transaction used when the RA8 must
# transform verified C6 response bytes before publishing a reader artifact.
# It compiles only the caller-buffer adapter and does not need protobuf or VFS.
add_executable(
  test_mdl_storage_ram ${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_mdl_storage_ram.c
                       ${FW_ROOT}/libs/ra8_c6link/src/ra8_mdl_storage_ram.c
)
target_compile_options(test_mdl_storage_ram PRIVATE -Wall -Wextra -Werror)
target_include_directories(test_mdl_storage_ram PRIVATE ${RA8_C6LINK_INCLUDE_DIRS})
add_test(NAME test_mdl_storage_ram COMMAND test_mdl_storage_ram)

# test_app_media_download_format: the target app's portable source-image
# formatter, exercised with production raster, book, compressor, and reader
# implementations. No C6 transport or target-only peripheral is linked here.
add_executable(
  test_app_media_download_format
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/src/test_app_media_download_format.c
  ${FW_ROOT}/examples/ek_ra8d2/hw_pending/media_download/src/media_download_format.c
  $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_app_media_download_format PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_app_media_download_format PRIVATE -Wall -Wextra -Werror)
target_include_directories(
  test_app_media_download_format
  PRIVATE ${FW_ROOT}/examples/ek_ra8d2/hw_pending/media_download/inc
          ${FW_ROOT}/apps/shared_libs/book/inc
          ${FW_ROOT}/libs/ra8_io/inc
          ${FW_ROOT}/apps/shared_libs/compress/inc
          ${FW_ROOT}/libs/ra8_mem/inc
          ${FW_ROOT}/apps/shared_libs/rabook_compile/inc
          ${FW_ROOT}/apps/shared_libs/reflow/inc
          ${FW_ROOT}/apps/shared_libs/webp/inc
          ${FW_ROOT}/apps/shared_libs/third_party/miniz
          ${RA8_C6LINK_INCLUDE_DIRS}
)
add_test(NAME test_app_media_download_format COMMAND test_app_media_download_format)

# test_ra8_c6link_rabook: the full mixed-image acceptance path. A generated
# RBKC artifact crosses the portable C6 service and RA client, is written to a
# real FAT/VFS transaction, strictly validated before publication, then reopened
# through the same bounded reader workspace for consumption. The C6 model
# decodes and answers the generated inner/outer protobuf messages; it is not a
# replay or a direct call around the transport.
add_executable(
  test_ra8_c6link_rabook
  ${FW_ROOT}/apps/shared_libs/rabook_compile/tests/src/test_ra8_c6link_rabook.c
  ${RA8_C6LINK_TEST_SUPPORT}
  ${FW_ROOT}/apps/shared_libs/rabook_compile/tests/src/rabook_compile_test_fixture.c
  ${RA8_C6LINK_TEST_MODEL}
  ${FW_ROOT}/apps/shared_libs/mdl_storage_vfs/src/mdl_storage_vfs.c
  ${FW_ROOT}/apps/shared_libs/mdl_storage_vfs/src/mdl_rabook_vfs.c
  ${RA8_C6LINK_SOURCES}
  ${RA8_C6LINK_SOUP}
  $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_c6link_rabook PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_ra8_c6link_rabook PRIVATE -Wall -Wextra -Werror)
target_include_directories(
  test_ra8_c6link_rabook
  PRIVATE ${RA8_C6LINK_INCLUDE_DIRS}
          ${FW_ROOT}/apps/shared_libs/mdl_storage_vfs/inc
          ${FW_ROOT}/apps/shared_libs/book/inc
          ${FW_ROOT}/apps/shared_libs/rabook_compile/inc
          ${FW_ROOT}/apps/shared_libs/rabook_compile/tests/inc
          ${FW_ROOT}/libs/ra8_fs/inc
          ${FW_ROOT}/libs/ra8_hal/inc
          ${FW_ROOT}/libs/ra8_io/inc
          ${FW_ROOT}/apps/shared_libs/compress/inc
          ${FW_ROOT}/libs/ra8_mem/inc
          ${FW_ROOT}/apps/shared_libs/third_party/miniz
)
add_test(NAME test_ra8_c6link_rabook COMMAND test_ra8_c6link_rabook)

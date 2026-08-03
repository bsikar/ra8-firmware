# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# ra8_wifi ESP32-C6 backend host test: the facade + the c6 backend + the real
# ra8_c6link stack + the vendored protobuf codec, driven end to end against the
# co-processor model tests/mocks/ra8_c6_model.c.
#
# This cannot come from the ra8_add_test() auto-glob for the same reason the
# c6link tests cannot (tests_c6link.cmake): the backend rides ra8_c6link, which
# encodes/decodes the vendored esp-hosted `Rpc` protobuf, so it needs the
# generated codec + the protobuf-c runtime + the esp-hosted include path, none
# of which ra8_core_hal carries. The pure facade (src/ra8_wifi.c) is a different
# story -- it names no radio, so it is in ra8_core_hal and its mock-backed test
# (test_ra8_wifi.c) rides the auto-glob.
#
# ra8_core_hal is linked in for ra8_err / ra8_log / ra8_check, and it already
# carries the compiled facade, so this target adds only the c6 backend TU.
#
# Included from tests/CMakeLists.txt. Depends on variables defined in
# tests_c6link.cmake (RA8_C6LINK_SOUP / RA8_C6LINK_SOURCES / include dirs), so it
# is included after it in the driver.

# The ESP32-C6 backend translation unit, compiled alongside the model.
set(RA8_WIFI_C6_BACKEND ${FW_ROOT}/libs/ra8_wifi/src/ra8_wifi_c6link.c)

set(RA8_WIFI_INCLUDE_DIRS ${RA8_C6LINK_INCLUDE_DIRS} ${FW_ROOT}/libs/ra8_wifi/inc
                          ${FW_ROOT}/libs/ra8_wifi/src
)

# test_ra8_wifi_c6link: the whole facade through the real c6 backend and the real
# ra8_c6link, answered by tests/mocks/ra8_c6_model.c. The model decodes what the
# host transmits with the same generated codec the ESP32-C6 runs and synthesises
# the answers and the station events a join produces, so association is exercised
# for real, with no hardware.
add_executable(
  test_ra8_wifi_c6link
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_wifi_c6link.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra8_c6_model.c
  ${RA8_WIFI_C6_BACKEND}
  ${RA8_C6LINK_SOURCES}
  ${RA8_C6LINK_SOUP}
  $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_wifi_c6link PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_ra8_wifi_c6link PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_include_directories(test_ra8_wifi_c6link PRIVATE ${RA8_WIFI_INCLUDE_DIRS})
add_test(NAME test_ra8_wifi_c6link COMMAND test_ra8_wifi_c6link)

# test_app_wifi_hal_join: the wifi_hal_join EXAMPLE's core wiring on the host.
# Compiles the SAME core (wifi_hal_core.c) the ARM example runs, driven against a
# mock ra8_wifi backend + a canned IP provider, so it proves the example reaches
# a bound IP and selects its PASS line. It needs neither ra8_c6link nor the codec
# -- only the facade in ra8_core_hal -- but is registered by hand (not the
# auto-glob) because it must also compile the example core and add the example's
# include directory. Same "example core on the host" shape as
# test_cache_store_demo.
set(RA8_WIFI_APP_DIR ${FW_ROOT}/examples/ek_ra8d2/hw_validated/c6/wifi_hal_join)
add_executable(
  test_app_wifi_hal_join ${CMAKE_CURRENT_SOURCE_DIR}/test_app_wifi_hal_join.c
                         ${RA8_WIFI_APP_DIR}/src/wifi_hal_core.c $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_app_wifi_hal_join PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_app_wifi_hal_join PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_include_directories(
  test_app_wifi_hal_join PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FW_ROOT}/libs/ra8_core/inc
                                 ${FW_ROOT}/libs/ra8_wifi/inc ${RA8_WIFI_APP_DIR}
)
add_test(NAME test_app_wifi_hal_join COMMAND test_app_wifi_hal_join)

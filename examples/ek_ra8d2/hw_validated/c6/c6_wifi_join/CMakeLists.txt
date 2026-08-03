#
# Per-app CMake stub for `c6_wifi_join`. The shared cmake/ra8_add_app.cmake
# recipe does the heavy lifting; this file names the app + its deps, then
# splices in the C6 NetX Duo driver, the vendored DHCP client and the
# build-time Wi-Fi credentials.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(c6_wifi_join LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# USES pulls in ThreadX, NetX Duo (the IP stack the DHCP client sits on) and the
# vendored esp-hosted host driver plus its RA8 port. LIBS globs the board layer
# in so the app reads its pin map from the board, plus ra8_c6link -- the facade
# the join runs through. `USES netxduo` also links the on-chip Ethernet driver
# (`netxduo_port_ra8_eth`); that driver is dead-stripped here because this app
# names `nx_ether_driver_c6` to `nx_ip_create` instead.
ra8_add_app(
  NAME c6_wifi_join
  STACK_BYTES 8192
  DESCRIPTION "Join the bench Wi-Fi over the ESP32-C6 and get an IP by DHCP (#492)"
  USES threadx netxduo esp_hosted
  LIBS ra8_board_ek_ra8d2 ra8_c6link
)

if(TARGET c6_wifi_join.elf)
  # The wireless NetX Duo driver bridges onto ra8_c6link; the DHCP client is not
  # part of the netxduo core glob (it lives under addons/dhcp), so name it here.
  get_property(_c6_nx_src GLOBAL PROPERTY RA8_NETXDUO_C6_PORT_SOURCES)
  set(_dhcp_src "${_d}/libs/third_party/netxduo/addons/dhcp/nxd_dhcp_client.c")
  target_sources(c6_wifi_join.elf PRIVATE ${_c6_nx_src} "${_dhcp_src}")
  target_include_directories(
    c6_wifi_join.elf PRIVATE "${_d}/port/netxduo/inc" "${_d}/libs/third_party/netxduo/addons/dhcp"
  )
  target_link_libraries(c6_wifi_join.elf PRIVATE netxduo_port_c6)
  # Vendored SOUP; silence its warnings the way cmake/netxduo.cmake does.
  set_source_files_properties("${_dhcp_src}" PROPERTIES COMPILE_OPTIONS "-w")

  # Wi-Fi credentials, injected from the environment at configure time. Empty by
  # default so the aggregate cross-build stays green and no credential is ever
  # committed; the runtime prints a FAIL when the SSID is empty.
  set(_c6_ssid "")
  set(_c6_psk "")
  if(DEFINED RA8_C6_WIFI_SSID)
    set(_c6_ssid "${RA8_C6_WIFI_SSID}")
  elseif(DEFINED ENV{RA8_C6_WIFI_SSID})
    set(_c6_ssid "$ENV{RA8_C6_WIFI_SSID}")
  endif()
  if(DEFINED RA8_C6_WIFI_PSK)
    set(_c6_psk "${RA8_C6_WIFI_PSK}")
  elseif(DEFINED ENV{RA8_C6_WIFI_PSK})
    set(_c6_psk "$ENV{RA8_C6_WIFI_PSK}")
  endif()
  target_compile_definitions(
    c6_wifi_join.elf PRIVATE "RA8_C6_WIFI_SSID=\"${_c6_ssid}\"" "RA8_C6_WIFI_PSK=\"${_c6_psk}\""
  )
endif()

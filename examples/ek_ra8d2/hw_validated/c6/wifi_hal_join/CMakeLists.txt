# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Per-app CMake stub for `wifi_hal_join` -- the HAL-way twin of `c6_wifi_join`.
# The shared cmake/ra8_add_app.cmake recipe does the heavy lifting; this file
# names the app + its deps (adding ra8_wifi over ra8_c6link), then splices in the
# C6 NetX Duo driver, the vendored DHCP client and the build-time credentials.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(wifi_hal_join LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

# USES pulls in ThreadX, NetX Duo (the IP stack the DHCP provider sits on) and
# the vendored esp-hosted host driver plus its RA8 port. LIBS globs the board
# layer plus ra8_c6link (the link) and ra8_wifi (the facade this app is a demo
# of). The wireless NetX driver is named to nx_ip_create, so the on-chip
# Ethernet driver `USES netxduo` also links is dead-stripped.
ra8_add_app(
  NAME wifi_hal_join
  STACK_BYTES 8192
  DESCRIPTION "Join the bench Wi-Fi and get an IP by DHCP, through the ra8_wifi facade"
  USES threadx netxduo esp_hosted
  LIBS ra8_board_ek_ra8d2 ra8_c6link ra8_wifi
)

if(TARGET wifi_hal_join.elf)
  # The wireless NetX Duo driver bridges onto ra8_c6link; the DHCP client is not
  # part of the netxduo core glob (it lives under addons/dhcp), so name it here.
  get_property(_c6_nx_src GLOBAL PROPERTY RA8_NETXDUO_C6_PORT_SOURCES)
  set(_dhcp_src "${_d}/libs/third_party/netxduo/addons/dhcp/nxd_dhcp_client.c")
  target_sources(wifi_hal_join.elf PRIVATE ${_c6_nx_src} "${_dhcp_src}")
  target_include_directories(
    wifi_hal_join.elf PRIVATE "${_d}/port/netxduo/inc" "${_d}/libs/third_party/netxduo/addons/dhcp"
  )
  target_link_libraries(wifi_hal_join.elf PRIVATE netxduo_port_c6)
  # Vendored SOUP; silence its warnings the way cmake/netxduo.cmake does.
  set_source_files_properties("${_dhcp_src}" PROPERTIES COMPILE_OPTIONS "-w")

  # Wi-Fi credentials: read from environment / wifi.env, or auto-fetched from OpenBao
  # (secret/ra8d2/bench-network) if configured. Empty by default so aggregate cross-builds stay green.
  set(_c6_ssid "")
  set(_c6_psk "")
  set(_c6_source "none")
  if(DEFINED RA8_C6_WIFI_SSID)
    set(_c6_ssid "${RA8_C6_WIFI_SSID}")
    set(_c6_source "environment variable")
  elseif(DEFINED ENV{RA8_C6_WIFI_SSID})
    set(_c6_ssid "$ENV{RA8_C6_WIFI_SSID}")
    set(_c6_source "environment variable / wifi.env")
  endif()
  if(DEFINED RA8_C6_WIFI_PSK)
    set(_c6_psk "${RA8_C6_WIFI_PSK}")
  elseif(DEFINED ENV{RA8_C6_WIFI_PSK})
    set(_c6_psk "$ENV{RA8_C6_WIFI_PSK}")
  endif()

  # Fallback: query OpenBao for bench credentials if not explicitly supplied
  if(NOT _c6_psk AND EXISTS "${_d}/scripts/secrets/openbao_client.py")
    execute_process(
      COMMAND python3 "${_d}/scripts/secrets/openbao_client.py" get secret/ra8d2/bench-network bench_psk
      OUTPUT_VARIABLE _bao_psk
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE _bao_res
    )
    if(_bao_res EQUAL 0 AND _bao_psk)
      set(_c6_psk "${_bao_psk}")
      if(NOT _c6_ssid)
        set(_c6_ssid "ra8-bench")
      endif()
      set(_c6_source "OpenBao vault (secret/ra8d2/bench-network)")
    endif()
  endif()

  if(_c6_ssid AND _c6_psk)
    message(STATUS "[Wi-Fi] Compiled credentials: SSID='${_c6_ssid}' (source: ${_c6_source})")
  else()
    message(STATUS "[Wi-Fi] No credentials supplied (set RA8_C6_WIFI_SSID/PSK in env/wifi.env, or configure OpenBao)")
  endif()

  target_compile_definitions(
    wifi_hal_join.elf PRIVATE "RA8_C6_WIFI_SSID=\"${_c6_ssid}\"" "RA8_C6_WIFI_PSK=\"${_c6_psk}\""
  )
endif()

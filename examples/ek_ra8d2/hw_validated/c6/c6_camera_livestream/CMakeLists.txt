# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(c6_camera_livestream LANGUAGES C ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

set(_camera_dir "${_d}/examples/ek_ra8d2/hw_validated/hil/camera_capture")

ra8_add_app(
  NAME c6_camera_livestream
  STACK_BYTES 12288
  DESCRIPTION "Serve live OV5640 frames over the ESP32-C6 Wi-Fi link"
  USES threadx netxduo esp_hosted
  LIBS ra8_board_ek_ra8d2 ra8_c6link
  EXTRA_SRCS "${_camera_dir}/src/cam_ceu.c" "${_camera_dir}/src/cam_ov5640.c"
)

if(TARGET c6_camera_livestream.elf)
  # ThreadX passes this address to tx_application_define(). This application
  # owns every object statically and ignores that argument, matching the other
  # C6 examples whose local linker scripts provide the SDRAM origin here.
  target_link_options(
    c6_camera_livestream.elf PRIVATE "-Wl,--defsym=g_ra8_threadx_unused_memory_start=0x68000000"
  )
  get_property(_c6_nx_src GLOBAL PROPERTY RA8_NETXDUO_C6_PORT_SOURCES)
  set(_dhcp_src "${_d}/libs/third_party/netxduo/addons/dhcp/nxd_dhcp_client.c")
  target_sources(c6_camera_livestream.elf PRIVATE ${_c6_nx_src} "${_dhcp_src}")
  target_include_directories(
    c6_camera_livestream.elf PRIVATE "${_d}/port/netxduo/inc"
                                     "${_d}/libs/third_party/netxduo/addons/dhcp" "${_camera_dir}"
  )
  target_link_libraries(c6_camera_livestream.elf PRIVATE netxduo_port_c6)
  set_source_files_properties("${_dhcp_src}" PROPERTIES COMPILE_OPTIONS "-w")

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
  if(NOT _c6_psk AND EXISTS "${_d}/scripts/secrets/openbao_client.py")
    execute_process(
      COMMAND python3 "${_d}/scripts/secrets/openbao_client.py" get secret/ra8d2/bench-network
              bench_psk
      OUTPUT_VARIABLE _bao_psk
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
      RESULT_VARIABLE _bao_res
    )
    if(_bao_res EQUAL 0 AND _bao_psk)
      set(_c6_psk "${_bao_psk}")
      if(NOT _c6_ssid)
        set(_c6_ssid "ra8-bench")
      endif()
    endif()
  endif()
  string(REPLACE "\\" "\\\\" _c6_ssid_c "${_c6_ssid}")
  string(REPLACE "\"" "\\\"" _c6_ssid_c "${_c6_ssid_c}")
  string(REPLACE "\\" "\\\\" _c6_psk_c "${_c6_psk}")
  string(REPLACE "\"" "\\\"" _c6_psk_c "${_c6_psk_c}")
  set(_credentials_source "${CMAKE_CURRENT_BINARY_DIR}/generated/c6_cam_credentials.c")
  get_filename_component(_credentials_dir "${_credentials_source}" DIRECTORY)
  file(MAKE_DIRECTORY "${_credentials_dir}")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/c6_cam_credentials.c.in" "${_credentials_source}" @ONLY
  )
  target_sources(c6_camera_livestream.elf PRIVATE "${_credentials_source}")
endif()

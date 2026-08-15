# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

include_guard(GLOBAL)

# Configure one C6 camera server app with shared sources and generated credentials.
function(c6_camera_server_add_app)
  cmake_parse_arguments(
    APP
    ""
    "NAME;DESCRIPTION;ROOT"
    ""
    ${ARGN}
  )
  if(NOT APP_NAME
     OR NOT APP_DESCRIPTION
     OR NOT APP_ROOT
  )
    message(FATAL_ERROR "c6_camera_server_add_app requires NAME, DESCRIPTION, and ROOT")
  endif()

  set(_common "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
  ra8_add_app(
    NAME ${APP_NAME}
    STACK_BYTES 12288
    DESCRIPTION "${APP_DESCRIPTION}"
    USES threadx netxduo esp_hosted
    LIBS ra8_board_ek_ra8d2
         ra8_c6link
         ra8_audio
         ra8_camera
         ra8_jpeg
         ra8_ov5640
    EXTRA_SRCS "${_common}/src/c6_cam_app.c" "${_common}/src/c6_cam_audio.c"
               "${_common}/src/c6_cam_console.c" "${_common}/src/c6_cam_net.c"
  )

  if(NOT TARGET ${APP_NAME}.elf)
    return()
  endif()

  target_link_options(
    ${APP_NAME}.elf PRIVATE "-Wl,--defsym=g_ra8_threadx_unused_memory_start=0x68000000"
  )
  get_property(_c6_nx_src GLOBAL PROPERTY RA8_NETXDUO_C6_PORT_SOURCES)
  set(_dhcp_src "${APP_ROOT}/libs/third_party/netxduo/addons/dhcp/nxd_dhcp_client.c")
  target_sources(${APP_NAME}.elf PRIVATE ${_c6_nx_src} "${_dhcp_src}")
  target_include_directories(
    ${APP_NAME}.elf PRIVATE "${_common}/inc" "${APP_ROOT}/port/netxduo/inc"
                            "${APP_ROOT}/libs/third_party/netxduo/addons/dhcp"
  )
  target_link_libraries(${APP_NAME}.elf PRIVATE netxduo_port_c6)
  set_source_files_properties("${_dhcp_src}" PROPERTIES COMPILE_OPTIONS "-w")

  set(_ssid "")
  set(_psk "")
  if(DEFINED RA8_C6_WIFI_SSID)
    set(_ssid "${RA8_C6_WIFI_SSID}")
  elseif(DEFINED ENV{RA8_C6_WIFI_SSID})
    set(_ssid "$ENV{RA8_C6_WIFI_SSID}")
  endif()
  if(DEFINED RA8_C6_WIFI_PSK)
    set(_psk "${RA8_C6_WIFI_PSK}")
  elseif(DEFINED ENV{RA8_C6_WIFI_PSK})
    set(_psk "$ENV{RA8_C6_WIFI_PSK}")
  endif()
  if(NOT _psk AND EXISTS "${APP_ROOT}/scripts/secrets/openbao_client.py")
    execute_process(
      COMMAND python3 "${APP_ROOT}/scripts/secrets/openbao_client.py" get secret/ra8d2/bench-network
              bench_psk
      OUTPUT_VARIABLE _bao_psk
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
      RESULT_VARIABLE _bao_res
    )
    if(_bao_res EQUAL 0 AND _bao_psk)
      set(_psk "${_bao_psk}")
      if(NOT _ssid)
        set(_ssid "ra8-bench")
      endif()
    endif()
  endif()
  string(REPLACE "\\" "\\\\" _ssid_c "${_ssid}")
  string(REPLACE "\"" "\\\"" _ssid_c "${_ssid_c}")
  string(REPLACE "\\" "\\\\" _psk_c "${_psk}")
  string(REPLACE "\"" "\\\"" _psk_c "${_psk_c}")
  set(_credentials "${CMAKE_CURRENT_BINARY_DIR}/generated/c6_cam_credentials.c")
  get_filename_component(_credentials_dir "${_credentials}" DIRECTORY)
  file(MAKE_DIRECTORY "${_credentials_dir}")
  set(_c6_ssid_c "${_ssid_c}")
  set(_c6_psk_c "${_psk_c}")
  configure_file("${_common}/c6_camera_server_credentials.c.in" "${_credentials}" @ONLY)
  target_sources(${APP_NAME}.elf PRIVATE "${_credentials}")
endfunction()

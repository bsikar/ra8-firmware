# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

include_guard(GLOBAL)

# Configure one C6 camera server app with shared sources and runtime provisioning.
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
         ra8_ov5640
    EXTRA_SRCS "${_common}/src/c6_cam_app.c"
               "${_common}/src/c6_cam_audio.c"
               "${_common}/src/c6_cam_console.c"
               "${_common}/src/c6_cam_net.c"
               "${APP_ROOT}/examples/ek_ra8d2/common/network_provision/src/ra8_net_provision.c"
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
    ${APP_NAME}.elf
    PRIVATE "${_common}/inc" "${APP_ROOT}/examples/ek_ra8d2/common/network_provision/inc"
            "${APP_ROOT}/port/netxduo/inc" "${APP_ROOT}/libs/third_party/netxduo/addons/dhcp"
  )
  target_link_libraries(${APP_NAME}.elf PRIVATE netxduo_port_c6)
  # Vendored SOUP. All three names were measured on nxd_dhcp_client.c under the
  # pinned cross toolchain arm-none-eabi-gcc 13.3.1 at -O0 by removing one at a
  # time with the others still applied: -Wdiscarded-qualifiers fires where the
  # DHCP option walker assigns a const UCHAR* into a plain UCHAR*,
  # -Wcast-align on its four-byte option reads through a UCHAR*, and
  # -Wcast-qual on the same const-dropping cast. None is redundant with
  # cmake/netxduo.cmake: that object library has no warning profile at all,
  # whereas this TU is compiled INTO the app and therefore meets the full
  # -Wall -Wextra -Werror project profile.
  set(_dhcp_warnings
      -Wno-discarded-qualifiers # NetX Duo DHCP assigns a const option cursor to its mutable cursor.
      -Wno-cast-align # NetX Duo DHCP reads four-byte options through its byte cursor.
      -Wno-cast-qual # NetX Duo DHCP deliberately drops const from its option cursor.
  )
  set_source_files_properties("${_dhcp_src}" PROPERTIES COMPILE_OPTIONS "${_dhcp_warnings}")
endfunction()

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Cross-build the EK-RA8D2 C6-to-SD strict .rabook composition root.

cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(media_download LANGUAGES C ASM)
endif()

set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME media_download
  STACK_BYTES 8192
  DESCRIPTION "C6 HTTPS to transactional SD strict .rabook publish and read (#665)"
  USES threadx esp_hosted
  LIBS ra8_board_ek_ra8d2
       ra8_c6link
       ra8_mdl_storage_vfs
       ra8_io
       ra8_fs
       ra8_sdmmc_spi
       ra8_book
       miniz
)

if(TARGET media_download.elf)
  set(_media_url "")
  set(_media_ssid "")
  set(_media_psk "")
  if(DEFINED RA8_MEDIA_DOWNLOAD_URL)
    set(_media_url "${RA8_MEDIA_DOWNLOAD_URL}")
  elseif(DEFINED ENV{RA8_MEDIA_DOWNLOAD_URL})
    set(_media_url "$ENV{RA8_MEDIA_DOWNLOAD_URL}")
  endif()
  if(DEFINED RA8_C6_WIFI_SSID)
    set(_media_ssid "${RA8_C6_WIFI_SSID}")
  elseif(DEFINED ENV{RA8_C6_WIFI_SSID})
    set(_media_ssid "$ENV{RA8_C6_WIFI_SSID}")
  endif()
  if(DEFINED RA8_C6_WIFI_PSK)
    set(_media_psk "${RA8_C6_WIFI_PSK}")
  elseif(DEFINED ENV{RA8_C6_WIFI_PSK})
    set(_media_psk "$ENV{RA8_C6_WIFI_PSK}")
  endif()

  target_compile_definitions(
    media_download.elf
    PRIVATE "RA8_MEDIA_DOWNLOAD_URL=\"${_media_url}\""
            "RA8_C6_WIFI_SSID=\"${_media_ssid}\""
            "RA8_C6_WIFI_PSK=\"${_media_psk}\""
            MINIZ_NO_MALLOC
            MINIZ_NO_STDIO
            MINIZ_NO_TIME
            MINIZ_NO_ARCHIVE_APIS
  )
endif()

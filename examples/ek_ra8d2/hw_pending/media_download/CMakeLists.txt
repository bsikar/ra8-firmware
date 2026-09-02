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
  DESCRIPTION "C6 HTTPS source to transactional SD strict .rabook publish and read (#665)"
  USES threadx esp_hosted
  LIBS ra8_board_ek_ra8d2
       ra8_c6link
       mdl_storage_vfs
       webp
       compress
       ra8_io
       ra8_fs
       ra8_sdmmc_spi
       book
       miniz
  EXTRA_SRCS ${_d}/apps/shared_libs/rabook_compile/src/ra8_rabook_comic.c
             ${_d}/apps/shared_libs/rabook_compile/src/rabook_compile.c
             ${_d}/apps/shared_libs/rabook_compile/src/ra8_rabook_container.c
             ${_d}/apps/shared_libs/rabook_compile/src/ra8_rabook_finalize.c
             ${_d}/apps/shared_libs/rabook_compile/src/ra8_rabook_gray4.c
             ${_d}/apps/shared_libs/rabook_compile/src/ra8_rabook_raster.c
             ${_d}/apps/shared_libs/reflow/src/ra8_img_arena.c
             ${_d}/examples/ek_ra8d2/common/network_provision/src/ra8_net_provision.c
             ${_d}/apps/shared_libs/third_party/stb/stb_image_impl.c
)

option(RA8_MEDIA_DOWNLOAD_SOURCE_IMAGE
       "Treat the C6 HTTPS body as one encoded image and format it as RABOOK" OFF
)

if(TARGET media_download.elf)
  target_include_directories(
    media_download.elf
    PRIVATE ${_d}/apps/shared_libs/rabook_compile/inc ${_d}/apps/shared_libs/reflow/inc
            ${_d}/examples/ek_ra8d2/common/network_provision/inc
            ${_d}/apps/shared_libs/third_party/stb
  )
  target_compile_definitions(
    media_download.elf
    PRIVATE "RA8_MEDIA_DOWNLOAD_SOURCE_IMAGE=$<BOOL:${RA8_MEDIA_DOWNLOAD_SOURCE_IMAGE}>"
            MINIZ_NO_MALLOC
            MINIZ_NO_STDIO
            MINIZ_NO_TIME
            MINIZ_NO_ARCHIVE_APIS
  )
endif()

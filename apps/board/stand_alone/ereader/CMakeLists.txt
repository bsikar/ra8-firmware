# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# CMakeLists.txt for the main e-reader application (apps/board/stand_alone/ereader).
#
# Configures a two-project TrustZone build:
# 1. ra8d2-ereader.elf (Secure Boot + Key Vault + NSC Veneers)
#    Note: This is named to match the app name so standard emulator
#          and flashing tools boot the Secure entry.
# 2. ra8d2-ereader_ns.elf (Non-Secure UI + ThreadX + Graphics)
#
#

cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(ra8d2-ereader LANGUAGES C CXX ASM)
endif()

# Dynamically resolve repository root directory
set(RA8_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${RA8_REPO_ROOT}/cmake/ra8_add_app.cmake" AND NOT "${RA8_REPO_ROOT}" STREQUAL "/")
  get_filename_component(RA8_REPO_ROOT "${RA8_REPO_ROOT}" DIRECTORY)
endwhile()

# Include the shared per-app build macro
include("${RA8_REPO_ROOT}/cmake/ra8_add_app.cmake")

# ---- OSPI XIP (execute-in-place) variant ----------------------------------
# RA8_EREADER_NS_XIP=ON links the Non-Secure image to run execute-in-place from
# OSPI flash (NS alias 0x9000_0000) instead of the default copy-to-SRAM run
# model: the Secure boot skips the MRAM->SRAM copy and BLXNS-es to OSPI, and
# ns_reset_handler copies only .data into SRAM. OFF by default so the proven
# SRAM-run e-reader stays the default build. Set before ra8_add_app() so the
# shared ns_usb_handoff Zig boot object receives trust_zone + ns_xip.
option(RA8_EREADER_NS_XIP "Run the Non-Secure e-reader XIP from OSPI flash" OFF)
set(RA8_APP_BOOT_TRUST_ZONE ON)
if(RA8_EREADER_NS_XIP)
  set(RA8_APP_BOOT_NS_XIP ON)
  set(_ra8_ns_xip XIP)
else()
  set(_ra8_ns_xip)
endif()

# ---- Secure Target --------------------------------------------------------
# Use the repository's standard macro to build the Secure side.
# This automatically handles include paths for ra8_core, ra8_hal, ra8_net_pal, ra8_usb_pal,
# ra8_board_ek_ra8d2, and compiles the Ring 5 libs/ra8_secure_app/ sources.
ra8_add_app(
  NAME ra8d2-ereader BOOT_PROFILE ns_usb_handoff
  STACK_BYTES 2200 MRAM_LENGTH 512K
  DESCRIPTION "Main e-reader Secure application (Bootloader + Vault)"
  LIBS ra8_mpu ra8_tz_secure_boot ra8_io_bus AUX_SRCS src/ns_main.c src/er_panel.c src/ns_pal_shims.c
)

# RA8_APP_UART_LOG mirrors ra8_log onto the SCI8 console (in ra8_nsc_periph_init) so
# the e-reader's logs appear on the UART -- ra8_emulator's on-screen console panel
# and a real serial terminal -- not only on the ITM/SWO trace.
target_compile_definitions(ra8d2-ereader.elf PRIVATE RA8_TRUSTZONE_ENABLE RA8_APP_UART_LOG)
target_compile_options(ra8d2-ereader.elf PRIVATE -mcmse -fshort-enums -ffreestanding)

# ---- Post-build: verify the NSC SG-veneer layout (T5-08) ------------------
# ra8d2-ereader_ns.elf is bound to the Secure-Gateway stubs through the CMSE import
# library emitted by ra8_add_ns_image(), so the byte offset of each veneer
# inside .gnu.sgstubs IS the S/NS ABI: a reorder redirects every NS->Secure
# call with no diagnostic. The board map derives the veneer bounds from the
# finished .gnu.sgstubs section, and this POST_BUILD step is what proves the
# slot layout the NS side is bound to. The checker requires one veneer per
# 8-byte slot across the whole region and self-skips a non-TrustZone link.
# --nm pins the SAME cross nm as the link (CMAKE_NM from the toolchain file).
add_custom_command(
  TARGET ra8d2-ereader.elf
  POST_BUILD
  COMMAND ${RA8_REPO_ROOT}/scripts/checks/check_sg_offsets.py $<TARGET_FILE:ra8d2-ereader.elf> --nm
          ${CMAKE_NM}
  VERBATIM
  COMMENT "check_sg_offsets: verifying NSC SG-veneer slot offsets (T5-08)"
)

# ---- Non-Secure Target ----------------------------------------------------
# Configures the Non-Secure ThreadX OS and the e-reader UI. ra8_add_ns_image()
# owns the CMSE implib handshake at both ends, the NS warning profile, the
# build ordering, and the merged-hex staple.
include(${RA8_REPO_ROOT}/cmake/threadx_ns.cmake)

# Bake the Latin-1 subset font into .rodata so the Reading body reflows real text
# with or without a microSD card present.
include("${RA8_REPO_ROOT}/cmake/ra8_zig_tool.cmake")
ra8_add_zig_host_tool(NAME font_to_c ZIG_ROOT "${RA8_REPO_ROOT}/tools/font_to_c")
set(_ra8_baked_font_c "${CMAKE_CURRENT_BINARY_DIR}/ra8_font_literata_latin1.c")
add_custom_command(
  OUTPUT "${_ra8_baked_font_c}"
  COMMAND
    $<TARGET_FILE:ra8_zig::font_to_c> "${RA8_REPO_ROOT}/libs/ra8_fonts/literata_latin1.ttf"
    "${_ra8_baked_font_c}" g_ra8_font_literata_latin1 literata_latin1.h
  DEPENDS "${RA8_REPO_ROOT}/libs/ra8_fonts/literata_latin1.ttf" "$<TARGET_FILE:ra8_zig::font_to_c>"
          ra8_zig_tool_font_to_c
  COMMENT "Generating baked Latin-1 font array (ra8d2-ereader)"
)

# The NS world runs the ThreadX watchdog supervisor. The supervisor is Zig
# now (libs/ra8_wdt_supervisor/src holds no .c), so its objects arrive as a
# static archive rather than as sources compiled into the image. It is named
# in LINK_LIBS ahead of threadx_ns because the supervisor calls into ThreadX:
# appended after it, _tx_time_get and the six _txe_* entry points go
# unresolved. The supervisor's default refresh hook takes
# ra8_wdt_refresh_deferred as a weak reference on silicon (RA8FW-891), so the
# NS image no longer compiles the HAL WDT driver: ns_main routes the hook
# through the ra8_nsc_wdt_refresh veneer instead.
ra8_zig_archive_for_cpu(
  _ra8_ns_wdt_sup
  LIB
  ra8_wdt_supervisor
  CPU
  cortex_m85
  FLOAT
  hard
)

ra8_zig_archive_for_cpu(
  _ra8_ns_display_pal
  LIB
  ra8_display_pal
  CPU
  cortex_m85
  FLOAT
  hard
)

ra8_zig_archive_for_cpu(
  _ra8_ns_gfx
  LIB
  ra8_gfx
  CPU
  cortex_m85
  FLOAT
  hard
)

ra8_zig_archive_for_cpu(
  _ra8_ns_zoom
  LIB
  zoom
  CPU
  cortex_m85
  FLOAT
  hard
  DIR
  apps/shared_libs/zoom
)

ra8_zig_archive_for_cpu(
  _ra8_ns_reflow
  LIB
  ra8_reflow_parse
  CPU
  cortex_m85
  FLOAT
  hard
  DIR
  apps/shared_libs/reflow
)

ra8_zig_archive_for_cpu(
  _ra8_ns_ui
  LIB
  ra8_ui
  CPU
  cortex_m85
  FLOAT
  hard
)

ra8_zig_archive_for_cpu(
  _ra8_ns_mem
  LIB
  ra8_mem
  CPU
  cortex_m85
  FLOAT
  hard
)

ra8_zig_archive_for_cpu(
  _ra8_ns_imgdec
  LIB
  ra8_imgdec
  CPU
  cortex_m85
  FLOAT
  hard
)


ra8_zig_archive_for_cpu(
  _ra8_ns_epub_fs
  LIB
  ra8_epub_fs
  CPU
  cortex_m85
  FLOAT
  hard
  DIR
  apps/shared_libs/epub
)

ra8_zig_archive_for_cpu(
  _ra8_ns_epub_open
  LIB
  ra8_epub_open
  CPU
  cortex_m85
  FLOAT
  hard
  DIR
  apps/shared_libs/epub
)

ra8_zig_archive_for_cpu(
  _ra8_ns_epub_chapter
  LIB
  ra8_epub_chapter
  CPU
  cortex_m85
  FLOAT
  hard
  DIR
  apps/shared_libs/epub
)

ra8_zig_archive_for_cpu(
  _ra8_ns_epub_entry
  LIB
  ra8_epub_entry
  CPU
  cortex_m85
  FLOAT
  hard
  DIR
  apps/shared_libs/epub
)

ra8_zig_archive_for_cpu(
  _ra8_ns_epub_xml
  LIB
  ra8_epub_xml
  CPU
  cortex_m85
  FLOAT
  hard
  DIR
  apps/shared_libs/epub
)

ra8_zig_archive_for_cpu(
  _ra8_ns_epub_toc
  LIB
  ra8_epub_toc
  CPU
  cortex_m85
  FLOAT
  hard
  DIR
  apps/shared_libs/epub
)

ra8_zig_archive_for_cpu(
  _ra8_ns_epub_miniz
  LIB
  ra8_epub_miniz
  CPU
  cortex_m85
  FLOAT
  hard
  DIR
  apps/shared_libs/epub
)

ra8_zig_archive_for_cpu(
  _ra8_ns_epub
  LIB
  ra8_epub
  CPU
  cortex_m85
  FLOAT
  hard
  DIR
  apps/shared_libs/epub
)

ra8_zig_archive_for_cpu(
  _ra8_ns_xml
  LIB
  xml_reader
  CPU
  cortex_m85
  FLOAT
  hard
  DIR
  apps/shared_libs/xml
)

ra8_zig_archive_for_cpu(
  _ra8_ns_core
  LIB
  ra8_core
  CPU
  cortex_m85
  FLOAT
  hard
)

ra8_zig_archive_for_cpu(
  _ra8_ns_io
  LIB
  ra8_io
  CPU
  cortex_m85
  FLOAT
  hard
)

ra8_zig_archive_for_cpu(
  _ra8_ns_fs
  LIB
  ra8_fs
  CPU
  cortex_m85
  FLOAT
  hard
)

ra8_zig_archive_for_cpu(
  _ra8_ns_hal
  LIB
  ra8_hal
  CPU
  cortex_m85
  FLOAT
  hard
  NS_ALIAS
)

ra8_add_ns_image(
  SECURE_TARGET
  ra8d2-ereader.elf
  NAME
  ra8d2-ereader_ns
  ${_ra8_ns_xip}
  STACK_BYTES
  2200
  IMPLIB
  ${CMAKE_CURRENT_BINARY_DIR}/ra8d2_cmse_import.o
  MERGED_HEX
  ra8d2-ereader.hex
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ns_main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/er_panel.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ns_pal_shims.c
  ${RA8_REPO_ROOT}/libs/ra8_hal/src/ra8_sdhi.c
  ${RA8_REPO_ROOT}/apps/shared_libs/third_party/miniz/miniz.c
  ${RA8_REPO_ROOT}/apps/shared_libs/third_party/stb/stb_image_impl.c
  ${RA8_REPO_ROOT}/apps/shared_libs/third_party/stb/stb_truetype_impl.c
  ${_ra8_baked_font_c}
  DEFINES
  RA8_TRUSTZONE_ENABLE
  RA8_PERIPH_NS_ALIAS
  RA8_USB_POLLED_ONLY
  MINIZ_NO_STDIO
  MINIZ_NO_TIME
  MINIZ_NO_MALLOC
  MINIZ_NO_DEFLATE_APIS
  MINIZ_NO_ARCHIVE_WRITING_APIS
  STBI_NO_GIF
  STBI_NO_BMP
  STBI_NO_PSD
  STBI_NO_TGA
  STBI_NO_PIC
  STBI_NO_PNM
  $<$<BOOL:${RA8_EREADER_NS_XIP}>:RA8_EREADER_NS_XIP>
  COMPILE_OPTIONS
  -Os
  INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/inc
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${RA8_REPO_ROOT}/libs/ra8_core/inc
  ${RA8_REPO_ROOT}/libs/ra8_ui/inc
  ${RA8_REPO_ROOT}/libs/ra8_mem/inc
  ${RA8_REPO_ROOT}/libs/ra8_imgdec/inc
  ${RA8_REPO_ROOT}/libs/ra8_jpeg/inc
  ${RA8_REPO_ROOT}/apps/shared_libs/webp/inc
  ${RA8_REPO_ROOT}/apps/shared_libs/third_party/stb
  ${RA8_REPO_ROOT}/libs/ra8_hal/inc
  ${RA8_REPO_ROOT}/libs/ra8_nsc/inc
  ${RA8_REPO_ROOT}/libs/ra8_board_ek_ra8d2/inc
  ${RA8_REPO_ROOT}/libs/ra8_wdt_supervisor/inc
  ${RA8_REPO_ROOT}/libs/ra8_display_pal/inc
  ${RA8_REPO_ROOT}/libs/ra8_gfx/inc
  ${RA8_REPO_ROOT}/libs/ra8_io/inc
  ${RA8_REPO_ROOT}/libs/ra8_fs/inc
  ${RA8_REPO_ROOT}/apps/shared_libs/zoom/inc
  ${RA8_REPO_ROOT}/apps/shared_libs/reflow/inc
  ${RA8_REPO_ROOT}/apps/shared_libs/reflow/src
  ${RA8_REPO_ROOT}/apps/shared_libs/epub/inc
  ${RA8_REPO_ROOT}/apps/shared_libs/epub/src
  ${RA8_REPO_ROOT}/apps/shared_libs/xml/inc
  ${RA8_REPO_ROOT}/apps/shared_libs/third_party/miniz
  ${RA8_REPO_ROOT}/examples/ek_ra8d2/hw_validated/hil/ereader_ui/inc
  ${RA8_REPO_ROOT}/libs/ra8_fonts
  ${RA8_REPO_ROOT}/libs/if/inc
  LINK_LIBS
  ${_ra8_ns_wdt_sup}
  ${_ra8_ns_display_pal}
  ${_ra8_ns_gfx}
  ${_ra8_ns_zoom}
  ${_ra8_ns_reflow}
  ${_ra8_ns_ui}
  ${_ra8_ns_epub_fs}
  ${_ra8_ns_epub_open}
  ${_ra8_ns_epub_chapter}
  ${_ra8_ns_epub_entry}
  ${_ra8_ns_epub_xml}
  ${_ra8_ns_epub_toc}
  ${_ra8_ns_epub_miniz}
  ${_ra8_ns_epub}
  ${_ra8_ns_xml}
  ${_ra8_ns_imgdec}
  ${_ra8_ns_mem}
  ${_ra8_ns_core}
  ${_ra8_ns_io}
  ${_ra8_ns_fs}
  ${_ra8_ns_hal}
  m
  gcc
  threadx_ns
)

set_property(
  SOURCE ${RA8_REPO_ROOT}/apps/shared_libs/third_party/miniz/miniz.c
         ${RA8_REPO_ROOT}/apps/shared_libs/third_party/stb/stb_image_impl.c
  APPEND
  PROPERTY COMPILE_OPTIONS "-w"
)

set_property(
  SOURCE ${RA8_REPO_ROOT}/apps/shared_libs/third_party/stb/stb_truetype_impl.c
  APPEND
  PROPERTY COMPILE_OPTIONS
    "-w"
    "-include"
    "${RA8_REPO_ROOT}/apps/shared_libs/reflow/inc/ra8_stbtt_alloc.h"
    "-DSTBTT_malloc(x,u)=ra8_stbtt_malloc(x)"
    "-DSTBTT_free(x,u)=ra8_stbtt_free(x)"
)

# The archive is produced by a rule in this directory and named by full path on
# the link line, so ninja already orders the two. State the edge anyway: the
# image is also reachable as a bare `ninja ra8d2-ereader_ns.elf`.
add_custom_target(ra8d2-ereader_ns_zig_wdt_supervisor DEPENDS ${_ra8_ns_wdt_sup})
add_custom_target(ra8d2-ereader_ns_zig_display_pal DEPENDS ${_ra8_ns_display_pal})
add_custom_target(ra8d2-ereader_ns_zig_gfx DEPENDS ${_ra8_ns_gfx})
add_custom_target(ra8d2-ereader_ns_zig_zoom DEPENDS ${_ra8_ns_zoom})
add_custom_target(ra8d2-ereader_ns_zig_reflow DEPENDS ${_ra8_ns_reflow})
add_custom_target(ra8d2-ereader_ns_zig_ui DEPENDS ${_ra8_ns_ui})
add_custom_target(ra8d2-ereader_ns_zig_mem DEPENDS ${_ra8_ns_mem})
add_custom_target(ra8d2-ereader_ns_zig_imgdec DEPENDS ${_ra8_ns_imgdec})
add_custom_target(ra8d2-ereader_ns_zig_epub_fs DEPENDS ${_ra8_ns_epub_fs})
add_custom_target(ra8d2-ereader_ns_zig_epub_open DEPENDS ${_ra8_ns_epub_open})
add_custom_target(ra8d2-ereader_ns_zig_epub_chapter DEPENDS ${_ra8_ns_epub_chapter})
add_custom_target(ra8d2-ereader_ns_zig_epub_entry DEPENDS ${_ra8_ns_epub_entry})
add_custom_target(ra8d2-ereader_ns_zig_epub_xml DEPENDS ${_ra8_ns_epub_xml})
add_custom_target(ra8d2-ereader_ns_zig_epub_toc DEPENDS ${_ra8_ns_epub_toc})
add_custom_target(ra8d2-ereader_ns_zig_epub_miniz DEPENDS ${_ra8_ns_epub_miniz})
add_custom_target(ra8d2-ereader_ns_zig_epub DEPENDS ${_ra8_ns_epub})
add_custom_target(ra8d2-ereader_ns_zig_xml DEPENDS ${_ra8_ns_xml})
add_custom_target(ra8d2-ereader_ns_zig_core DEPENDS ${_ra8_ns_core})
add_custom_target(ra8d2-ereader_ns_zig_io DEPENDS ${_ra8_ns_io})
add_custom_target(ra8d2-ereader_ns_zig_fs DEPENDS ${_ra8_ns_fs})
add_custom_target(ra8d2-ereader_ns_zig_hal DEPENDS ${_ra8_ns_hal})
add_dependencies(
  ra8d2-ereader_ns.elf
  ra8d2-ereader_ns_zig_wdt_supervisor
  ra8d2-ereader_ns_zig_display_pal
  ra8d2-ereader_ns_zig_gfx
  ra8d2-ereader_ns_zig_zoom
  ra8d2-ereader_ns_zig_reflow
  ra8d2-ereader_ns_zig_ui
  ra8d2-ereader_ns_zig_mem
  ra8d2-ereader_ns_zig_imgdec
  ra8d2-ereader_ns_zig_epub_fs
  ra8d2-ereader_ns_zig_epub_open
  ra8d2-ereader_ns_zig_epub_chapter
  ra8d2-ereader_ns_zig_epub_entry
  ra8d2-ereader_ns_zig_epub_xml
  ra8d2-ereader_ns_zig_epub_toc
  ra8d2-ereader_ns_zig_epub_miniz
  ra8d2-ereader_ns_zig_epub
  ra8d2-ereader_ns_zig_xml
  ra8d2-ereader_ns_zig_core
  ra8d2-ereader_ns_zig_io
  ra8d2-ereader_ns_zig_fs
  ra8d2-ereader_ns_zig_hal
)

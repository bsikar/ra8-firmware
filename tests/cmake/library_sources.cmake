# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Every first-party library source that goes into the ra8_core_hal object
# library, plus the vendored SOUP subsets the host build compiles alongside it.
#
# One place to answer "is this library in the host test build". A source glob
# that quietly stopped matching is invisible -- the build still succeeds, the
# library is simply never compiled or tidied.
#
# Included from tests/CMakeLists.txt. CMake include() is textual within the
# same directory scope, so every variable and target defined here is visible
# to the driver and to the fragments included after it.

# Firmware root (one directory up from tests/).
get_filename_component(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Collect every library source. Each .c file becomes part of the
# `ra8_core_hal` static library. clang-tidy then walks the associated
# compile_commands.json.
file(GLOB_RECURSE RA8_CORE_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_core/src/*.c)
file(GLOB_RECURSE RA8_HAL_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_hal/src/*.c)
file(GLOB_RECURSE RA8_NET_PAL_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_net_pal/src/*.c)
file(GLOB_RECURSE RA8_MODEM_AT_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_modem_at/src/*.c)
file(GLOB_RECURSE RA8_BLE_HOST_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_ble_host/src/*.c)
file(GLOB_RECURSE RA8_TLS_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_tls/src/*.c)
file(GLOB_RECURSE RA8_USB_PAL_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_usb_pal/src/*.c)
file(GLOB_RECURSE RA8_FS_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_fs/src/*.c)
file(GLOB_RECURSE RA8_IO_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_io/src/*.c)
file(GLOB_RECURSE RA8_FTL_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_ftl/src/*.c)
file(GLOB_RECURSE RA8_MEM_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_mem/src/*.c)
file(GLOB_RECURSE RA8_SDMMC_SPI_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_sdmmc_spi/src/*.c)
file(GLOB_RECURSE RA8_GFX_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_gfx/src/*.c)
file(GLOB_RECURSE RA8_UI_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_ui/src/*.c)
file(GLOB_RECURSE RA8_KEYBOARD_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_keyboard/src/*.c)
file(GLOB_RECURSE RA8_BOX_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_box/src/*.c)
file(GLOB_RECURSE RA8_BOOK_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_book/src/*.c)
file(GLOB_RECURSE RA8_RABOOK_COMPILE_SOURCES CONFIGURE_DEPENDS
     ${FW_ROOT}/libs/ra8_rabook_compile/src/*.c
)
file(GLOB_RECURSE RA8_RABOOK_COMPILE_CPP_SOURCES CONFIGURE_DEPENDS
     ${FW_ROOT}/libs/ra8_rabook_compile/src/*.cpp
)
file(GLOB_RECURSE RA8_RABOOK_IMPORT_SOURCES CONFIGURE_DEPENDS
     ${FW_ROOT}/libs/ra8_rabook_import/src/*.c
)
file(GLOB_RECURSE RA8_BATT_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_batt/src/*.c)
file(GLOB_RECURSE RA8_WIDGET_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_widget/src/*.c)
file(GLOB_RECURSE RA8_APP_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_app/src/*.c)
file(GLOB_RECURSE RA8_NSC_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_nsc/src/*.c)
file(GLOB_RECURSE RA8_OTA_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_ota/src/*.c)
file(GLOB_RECURSE RA8_DISPLAY_PAL_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_display_pal/src/*.c)
file(GLOB_RECURSE RA8_POWER_PROFILE_SOURCES CONFIGURE_DEPENDS
     ${FW_ROOT}/libs/ra8_power_profile/src/*.c
)
file(GLOB_RECURSE RA8_EPUB_C_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_epub/src/*.c)
file(GLOB_RECURSE RA8_EPUB_CPP_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_epub/src/*.cpp)
file(GLOB_RECURSE RA8_COMIC_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_comic/src/*.c)
file(GLOB_RECURSE RA8_UNARCH_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_unarch/src/*.c)
# xz-embedded decode-only SOUP: exactly the TUs the XZ wrapper drives. They
# include the first-party porting header libs/ra8_unarch/inc/xz_config.h
# (allocator seam + mode selection), so that include dir must stay on the
# ra8_core_hal PUBLIC list.
set(RA8_XZ_THIRD_PARTY
    ${FW_ROOT}/libs/third_party/xz_embedded/xz_crc32.c
    ${FW_ROOT}/libs/third_party/xz_embedded/xz_crc64.c
    ${FW_ROOT}/libs/third_party/xz_embedded/xz_dec_lzma2.c
    ${FW_ROOT}/libs/third_party/xz_embedded/xz_dec_stream.c
)
file(GLOB_RECURSE RA8_JOF_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_jof/src/*.c)
file(GLOB_RECURSE RA8_LONGSTRIP_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_longstrip/src/*.c)
# ra8_reflow has two implementations: v1 (hand-rolled, default) and v2
# (LiteHTML-backed, RA8_REFLOW_USE_LITEHTML=ON). Only one set is compiled.
option(RA8_REFLOW_USE_LITEHTML "Use the LiteHTML v2 ra8_reflow engine" OFF)
if(RA8_REFLOW_USE_LITEHTML)
  # The static stb_truetype arena allocator (ra8_stbtt_malloc/free) is shared:
  # both v1 and the litehtml v2 engine rasterise glyphs through stb_truetype,
  # so this one file stays in even when the v1 reflow engine is excluded.
  # ra8_stbtt_guard.c (the #217 sfnt table-directory bounds check) is likewise
  # shared: ra8_epub's priv_font_init calls it before stbtt_InitFont, so its
  # symbol must resolve even under the v2 engine.
  # ra8_img_arena.c (the stb_image bump arena) is likewise shared: the always-on
  # stb_image_impl.c TU in RA8_EPUB_THIRD_PARTY references its symbols.
  # ra8_reflow_link.c is pure query logic over engine fields (no layout/stbtt),
  # so the #110 link/anchor API is available under v2 as well.
  set(RA8_REFLOW_C_SOURCES
      ${FW_ROOT}/libs/ra8_reflow/src/ra8_stbtt_alloc.c
      ${FW_ROOT}/libs/ra8_reflow/src/ra8_stbtt_guard.c
      ${FW_ROOT}/libs/ra8_reflow/src/ra8_img_arena.c
      ${FW_ROOT}/libs/ra8_reflow/src/ra8_reflow_link.c
  )
  set(RA8_REFLOW_CPP_SOURCES ${FW_ROOT}/libs/ra8_reflow/v2/ra8_reflow_v2.cpp)
  enable_language(CXX)
  set(_RA8_SAVED_C_FLAGS "${CMAKE_C_FLAGS}")
  set(_RA8_SAVED_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  string(REPLACE "-Werror" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
  string(REPLACE "-Werror" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  if(NOT TARGET litehtml)
    add_subdirectory(
      ${FW_ROOT}/libs/third_party/litehtml ${CMAKE_BINARY_DIR}/_litehtml EXCLUDE_FROM_ALL
    )
  endif()
  if(TARGET litehtml)
    target_compile_options(litehtml PRIVATE -w)
  endif()
  if(TARGET gumbo)
    target_compile_options(gumbo PRIVATE -w)
  endif()
  set(CMAKE_C_FLAGS "${_RA8_SAVED_C_FLAGS}")
  set(CMAKE_CXX_FLAGS "${_RA8_SAVED_CXX_FLAGS}")
else()
  file(GLOB_RECURSE RA8_REFLOW_C_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_reflow/src/*.c)
  file(GLOB_RECURSE RA8_REFLOW_CPP_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_reflow/src/*.cpp)
endif()
set(RA8_EPUB_THIRD_PARTY
    ${FW_ROOT}/libs/third_party/miniz/miniz.c ${FW_ROOT}/libs/third_party/tinyxml2/tinyxml2.cpp
    ${FW_ROOT}/libs/third_party/stb/stb_truetype_impl.c
    ${FW_ROOT}/libs/third_party/stb/stb_image_impl.c
)
# libwebp decode-only SOUP (#290) + the first-party ra8_webp facade/arena that
# fronts it. Only the decoder subset is vendored under libs/third_party/libwebp,
# so a recursive *.c glob is exactly that subset. NOT yet wired into the
# ra8_reflow/ra8_img raster dispatch (that is #289) -- compiled into ra8_core_hal
# so the standalone WebP decode host test (test_ra8_webp.c) and the fuzz harness
# (fuzz_ra8_webp) link against it. utils.c routes its allocator through the
# ra8_webp bump arena via -DRA8_WEBP_USE_ARENA (set below).
include(${FW_ROOT}/cmake/ra8_webp_vendor.cmake)
ra8_webp_vendor_sources(RA8_WEBP_THIRD_PARTY ${FW_ROOT})
ra8_webp_facade_sources(RA8_WEBP_SOURCES ${FW_ROOT})
file(GLOB_RECURSE RA8_SECURE_APP_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/src/secure_app/*.c)
file(GLOB_RECURSE RA8_TOUCH_CAL_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_touch_cal/src/*.c)
file(GLOB_RECURSE RA8_EPD_CAL_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_epd_cal/src/*.c)
file(GLOB_RECURSE RA8_PSA_CRYPTO_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_psa_crypto/src/*.c)
file(GLOB_RECURSE RA8_WDT_SUPERVISOR_SOURCES CONFIGURE_DEPENDS
     ${FW_ROOT}/libs/ra8_wdt_supervisor/src/*.c
)
file(GLOB_RECURSE RA8_MPU_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_mpu/src/*.c)
file(GLOB_RECURSE RA8_BOARD_EK_RA8D2_SOURCES CONFIGURE_DEPENDS
     ${FW_ROOT}/libs/ra8_board_ek_ra8d2/src/*.c
)
file(GLOB_RECURSE RA8_LSM6DSO_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_lsm6dso/src/*.c)
file(GLOB_RECURSE RA8_TZ_SECURE_BOOT_SOURCES CONFIGURE_DEPENDS
     ${FW_ROOT}/libs/ra8_tz_secure_boot/src/*.c
)
file(GLOB_RECURSE RA8_DFU_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_dfu/src/*.c)
file(GLOB_RECURSE RA8_DEVCFG_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_devcfg/src/*.c)

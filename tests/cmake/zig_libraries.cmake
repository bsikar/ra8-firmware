# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Libraries whose implementation now lives in Zig behind their unchanged C
# header. Each one is built by `zig build` and linked into ra8_core_hal's
# consumers, so the existing C unit tests exercise the Zig object code without
# a single test edit.

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/zig_library.cmake)

ra8_add_zig_library(
  NAME
  ra8_box
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_box
  LIBRARY_NAME
  ra8_box
)

ra8_add_zig_library(
  NAME
  ra8_power_profile
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_power_profile
  LIBRARY_NAME
  ra8_power_profile
)

ra8_add_zig_library(
  NAME
  ra8_epd_cal
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_epd_cal
  LIBRARY_NAME
  ra8_epd_cal
)

ra8_add_zig_library(
  NAME
  ra8_touch_cal
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_touch_cal
  LIBRARY_NAME
  ra8_touch_cal
)

# Fully migrated: the record core and the production extra-MRAM store binding
# are both Zig now, so libs/ra8_devcfg/src has no .c left and the
# RA8_DEVCFG_SOURCES glob is gone from library_sources.cmake and core_hal.cmake.
ra8_add_zig_library(
  NAME
  ra8_devcfg
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_devcfg
  LIBRARY_NAME
  ra8_devcfg
)

# Fully migrated: the runtime path (put / get / read / evict / pin / sync /
# close) and the mount / recovery path both live in this archive, so
# tests_storage.cmake no longer globs any C sources for the library. The
# cache-store targets consume ra8_core_hal through $<TARGET_OBJECTS:>, which
# carries no link dependencies, so they link this archive by name in
# tests_storage.cmake rather than through the list below.
ra8_add_zig_library(
  NAME
  ra8_cache_store
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_cache_store
  LIBRARY_NAME
  ra8_cache_store
)

# Fully migrated: the edge + hysteresis nag policy is Zig now, so
# libs/ra8_batt/src has no .c left and the RA8_BATT_SOURCES glob is gone from
# library_sources.cmake and core_hal.cmake.
ra8_add_zig_library(
  NAME
  ra8_batt
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_batt
  LIBRARY_NAME
  ra8_batt
)

# Fully migrated: hit-testing, the screen-id stack and the page cursor are Zig
# now, so libs/ra8_ui/src has no .c left and the RA8_UI_SOURCES glob is gone
# from library_sources.cmake and core_hal.cmake.
ra8_add_zig_library(
  NAME
  ra8_ui
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_ui
  LIBRARY_NAME
  ra8_ui
)

# Fully migrated: the registry, the focus lifecycle, input / tick / render
# routing and the navigation back-stack are Zig now, so libs/ra8_app/src has no
# .c left and the RA8_APP_SOURCES glob is gone from library_sources.cmake and
# core_hal.cmake.
ra8_add_zig_library(
  NAME
  ra8_app
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_app
  LIBRARY_NAME
  ra8_app
)

# Fully migrated: the check-in registry, the deadline arithmetic, the refresh
# verdict and the ThreadX seam are Zig now, so libs/ra8_wdt_supervisor/src has
# no .c left and the RA8_WDT_SUPERVISOR_SOURCES glob is gone from
# library_sources.cmake and core_hal.cmake. The host-only ThreadX shim header
# remains for its focused C coverage test.
ra8_add_zig_library(
  NAME
  ra8_wdt_supervisor
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_wdt_supervisor
  LIBRARY_NAME
  ra8_wdt_supervisor
)

# Fully migrated: the descriptor validation, the AP / RBAR / RLAR encoding and
# the canonical 5-region boot attribute map are Zig now, so libs/ra8_mpu/src
# has no .c left and the RA8_MPU_SOURCES glob is gone from
# library_sources.cmake and core_hal.cmake.
ra8_add_zig_library(
  NAME
  ra8_mpu
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_mpu
  LIBRARY_NAME
  ra8_mpu
)

# Fully migrated: the frame ring, the ra8_eth status translation and the
# event fan-out are Zig now, so libs/ra8_net_pal/src has no .c left and the
# RA8_NET_PAL_SOURCES glob is gone from library_sources.cmake and
# core_hal.cmake. The Ring-3 ra8_eth driver stays a link-time seam, so the
# host fake Ethernet fixture substitutes for it exactly as before.
ra8_add_zig_library(
  NAME
  ra8_net_pal
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_net_pal
  LIBRARY_NAME
  ra8_net_pal
)

# Fully migrated: the CTRL1_XL / CTRL2_G bit-field encoders, the little-endian
# sample decoders, the temperature conversion and the FIFO drain are Zig now,
# so libs/ra8_lsm6dso/src has no .c left and the RA8_LSM6DSO_SOURCES glob is
# gone from library_sources.cmake and core_hal.cmake. The transport stays a
# caller-supplied seam, so the host suite's canned-response mock substitutes
# for the I2C / SPI bus exactly as before.
ra8_add_zig_library(
  NAME
  ra8_lsm6dso
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_lsm6dso
  LIBRARY_NAME
  ra8_lsm6dso
)

# Partially migrated. The per-endpoint packet rings, the two MC/DC-promoted
# predicates, the ra8_usb status -> PAL event translation and the whole public
# ra8_usb_pal.h surface are Zig now, so src/ra8_usb_pal.c is gone. The
# four descriptor builders behind inc/ra8_usb_desc.h are Zig too, so
# src/ra8_usb_desc.c is gone. The RA8_USB_PAL_SOURCES glob STAYS:
# ra8_usb_compose.c, the one-call facade dev layered over those builders
# (#766), is still C. src/ra8_usb_pal_internal.h stays
# too: the host suite includes it to drive the two promoted predicates, which
# the Zig archive exports under the same names. The Ring-3 ra8_usb driver
# stays a link-time seam.
ra8_add_zig_library(
  NAME
  ra8_usb_pal
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_usb_pal
  LIBRARY_NAME
  ra8_usb_pal
)

# Fully migrated: the half-unit key grid, the letters / numbers / symbols
# layers, the hit index and the typing model are Zig now, so
# libs/ra8_keyboard/src has no .c left and the RA8_KEYBOARD_SOURCES glob is
# gone from library_sources.cmake and core_hal.cmake. The rectangle test stays
# owned by ra8_ui: this archive calls ra8_ui_rect_contains as an external
# symbol, so the link dependency below is declared rather than left to the
# order of the list further down.
ra8_add_zig_library(
  NAME
  ra8_keyboard
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_keyboard
  LIBRARY_NAME
  ra8_keyboard
)

# Fully migrated: the transport-neutral facade, the fixed in-memory replay
# backend and the PDM-IF backend are all Zig now, so libs/ra8_audio/src holds
# no .c and the RA8_AUDIO_SOURCES glob is gone from library_sources.cmake and
# core_hal.cmake. The private vtable header src/ra8_audio_internal.h stays,
# because tests/misc/src/test_ra8_audio.c includes it to build its own fake
# backend; the libs/ra8_audio/src include dirs in core_hal.cmake and
# unit_tests.cmake are still needed for it. The PDM HAL and the millisecond
# clock stay link-time externs, so tests/hal/src/test_ra8_pdm.c substitutes
# its fixture exactly as it did for the C.
ra8_add_zig_library(
  NAME
  ra8_audio
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_audio
  LIBRARY_NAME
  ra8_audio
)

# Fully migrated: the lifecycle state machine, the backend-table validation,
# the bounded association wait and the lease rule went first, and the ESP32-C6
# backend followed, so libs/ra8_wifi/src has no .c left and the
# RA8_WIFI_SOURCES entry is gone from library_sources.cmake and core_hal.cmake.
# The backend is src/ra8_wifi_c6link_abi.zig over src/internal/c6link.zig; it
# exports k_ra8_wifi_backend_c6link and ra8_wifi_c6link_setup and calls the
# ra8_c6link entry points the C called, so the c6 host test still needs that
# stack plus the vendored protobuf codec and keeps its own target in
# tests_wifi.cmake. The radio stays a caller-supplied vtable, so the host
# suite's mock backend substitutes for it exactly as before. The two wifi
# targets in tests_wifi.cmake consume ra8_core_hal through
# $<TARGET_OBJECTS:>, which carries no link dependencies, so they link this
# archive by name there rather than through the list below.
# Partially migrated: the OV5640 register protocol, the board-qualified VGA DVP
# scene table, the JPEG overlay writes and the JPEG status decode are Zig now,
# so libs/ra8_ov5640/src/ra8_ov5640.c is gone. The SCCB transport and the
# millisecond delay stay caller-injected seams, so this archive links against
# no RA8 peripheral. src/ra8_ov5640_bind.c is deliberately still C: it is the
# house-I2C binder dev added in #760 after this port was written, an adapter
# over ra8_i2c_bus_ops_t rather than part of the driver being ported, so the
# RA8_OV5640_SOURCES glob stays in library_sources.cmake and core_hal.cmake
# exactly as ra8_lsm6dso's binder does.
ra8_add_zig_library(
  NAME
  ra8_ov5640
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_ov5640
  LIBRARY_NAME
  ra8_ov5640
)

# Fully migrated: the AT line accumulator, the OK / ERROR / +CME ERROR /
# +CMS ERROR / BUSY / NO CARRIER final-result table, the newline-separated
# capture appender and the eight-slot URC dispatch table are all Zig now, so
# libs/ra8_modem_at/src has no .c left and the RA8_MODEM_AT_SOURCES glob is
# gone from library_sources.cmake and core_hal.cmake. The byte transport and
# the millisecond timebase stay caller-injected seams, so the archive links
# against no driver. The eight priv_modem_* helpers declared in
# src/ra8_modem_at_internal.h are driven by the MC/DC suites. They are emitted
# only by a test-configured copy of the archive (-Dtest-helpers=true), which
# carries the public ABI too, so the host tests link that copy alone and the
# plain archive below is the one the target build consumes. Linking both would
# define every ra8_modem_at_* symbol twice, which is why ra8_zig::ra8_modem_at
# is deliberately absent from the two link lists further down.
ra8_add_zig_library(
  NAME
  ra8_modem_at
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_modem_at
  LIBRARY_NAME
  ra8_modem_at
)

set(_ra8_modem_at_test_dir "${CMAKE_CURRENT_BINARY_DIR}/zig_libs/ra8_modem_at_test_helpers")
set(_ra8_modem_at_test_library
    "${_ra8_modem_at_test_dir}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}ra8_modem_at_test_helpers${CMAKE_STATIC_LIBRARY_SUFFIX}"
)
add_custom_target(
  ra8_modem_at_test_helpers_zig_library ALL
  COMMAND "${ZIG_EXECUTABLE}" build -Dtest-helpers=true -Doptimize=Debug --prefix
          "${_ra8_modem_at_test_dir}" --cache-dir "${_ra8_modem_at_test_dir}/cache"
          --global-cache-dir "${_ra8_modem_at_test_dir}/global-cache"
  WORKING_DIRECTORY "${FW_ROOT}/libs/ra8_modem_at"
  BYPRODUCTS "${_ra8_modem_at_test_library}"
  COMMENT "Building test-only ra8_modem_at helper archive"
  VERBATIM
)
add_library(ra8_zig::ra8_modem_at_test_helpers STATIC IMPORTED GLOBAL)
set_target_properties(
  ra8_zig::ra8_modem_at_test_helpers
  PROPERTIES IMPORTED_LOCATION "${_ra8_modem_at_test_library}"
             INTERFACE_INCLUDE_DIRECTORIES "${FW_ROOT}/libs/ra8_modem_at/inc;${FW_ROOT}/libs/ra8_modem_at/src"
)
add_dependencies(ra8_zig::ra8_modem_at_test_helpers ra8_modem_at_test_helpers_zig_library)
link_libraries(ra8_zig::ra8_modem_at_test_helpers)

ra8_add_zig_library(
  NAME
  ra8_wifi
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_wifi
  LIBRARY_NAME
  ra8_wifi
)

# Fully migrated: the fw_if_fs adapter over ra8_io_vfs is Zig now, so
# libs/if_ra8_vfs/src has no .c left and the RA8_IF_RA8_VFS_SOURCES glob is
# gone from library_sources.cmake and core_hal.cmake. The private contracts
# header went with the .c, so libs/if_ra8_vfs/src is no longer an include
# directory anywhere; only inc/ remains. fw_fs_ra8_vfs_init stays the one
# exported symbol, with the namespace, stream and transaction vtables
# file-private exactly as static made them.
ra8_add_zig_library(
  NAME
  if_ra8_vfs
  ZIG_ROOT
  ${FW_ROOT}/libs/if_ra8_vfs
  LIBRARY_NAME
  if_ra8_vfs
)

# libs/ra8_camera keeps one C translation unit: src/ra8_camera_source_ceu.c is
# white-boxed by tests/misc/src/test_ra8_camera.c and test_ra8_ceu_cov.c with
# `#include "ra8_camera_source_ceu.c"`, so it stays C and binds the same private
# vtable from src/ra8_camera_internal.h (which stays for the same reason). The
# facade, the fixed-frame memory source and both codecs are Zig, so the
# RA8_CAMERA_SOURCES glob and the libs/ra8_camera/src include directory both
# stay in place for the one remaining file.
ra8_add_zig_library(
  NAME
  ra8_camera
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_camera
  LIBRARY_NAME
  ra8_camera
)

# Fully migrated: the portable filesystem facade AND the untrusted-name
# containment policy (#749) are both Zig now, so src/ra8_path.c is gone, the
# RA8_IF_SOURCES glob is gone from library_sources.cmake and core_hal.cmake,
# and libs/if/src is no longer an include directory anywhere. The three
# ra8_path_* symbols ride in this same archive behind the unchanged
# inc/ra8_path.h, which is why apps/shared_libs/mdl no longer compiles that
# .c into eight of its targets: it already links ra8_zig::fw_if_fs.
ra8_add_zig_library(
  NAME
  fw_if_fs
  ZIG_ROOT
  ${FW_ROOT}/libs/if
  LIBRARY_NAME
  fw_if_fs
)

# Fully migrated: the FTL core (init, the presented free-overwrite vtable,
# copy-on-write relocation, reclamation and wear-levelling), the canonical
# checkpoint codec and the mount lifecycle are all Zig, so ra8_ftl.c,
# ra8_ftl_checkpoint.c and ra8_ftl_mount.c are gone and there is no
# RA8_FTL_SOURCES glob left. The archive calls the ra8_io_blockdev_* front
# door, which lives in ra8_core_hal's own objects.
ra8_add_zig_library(
  NAME
  ra8_ftl
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_ftl
  LIBRARY_NAME
  ra8_ftl
)
set_property(
  TARGET ra8_zig::ra8_keyboard
  APPEND
  PROPERTY INTERFACE_LINK_LIBRARIES ra8_zig::ra8_ui
)

# Fully migrated: every hand-written translation unit of the rasteriser is Zig
# now -- the single shared framebuffer binding g_gfx_text_state, the four
# promoted helpers priv_gfx_text_pack_565 / priv_gfx_text_plot / priv_gfx_bpp /
# priv_gfx_format_ok, the fifteen entry points of inc/ra8_gfx.h with the two
# bind forms, the teardown, the packed-gray4 loupe zoom blit and both text
# calls among them, the three inc/ra8_gfx_tone.h calls with their committed
# nominal curve, and the six inc/ra8_gfx_dither.h calls over the committed
# blue-noise mask. The one C translation unit left in the library (the
# generated ra8_gfx_font_8x16.c) reaches into this archive through the
# unchanged src/ra8_gfx_internal.h, so that header and the libs/ra8_gfx/src
# include dirs in core_hal.cmake and unit_tests.cmake all stay.
ra8_add_zig_library(
  NAME
  ra8_gfx
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_gfx
  LIBRARY_NAME
  ra8_gfx
)

# ra8_core_hal is the OBJECT library every host test links, so an INTERFACE
# link here reaches each test executable that pulls in a migrated library.
# Fully migrated: the protocol core (both CRC generators, command framing,
# the R1/R3/R7 response readers, the bounded waits, the transport gate and
# the CMD0..CMD16 identification sequence) and the block-I/O TU (init/deinit,
# single- and multi-block read/write, erase, the capacity and card-type
# queries, the ra8_fs backend adapter and the SCI Simple-SPI transport
# factory) are both Zig, and the archive owns the sole definition of
# g_sdmmc_spi_state. The archive reaches ra8_sci_spi_*, ra8_gpio_* and
# ra8_pfs_route_peripheral as externs that ra8_core_hal's own objects supply.
# src/ra8_sdmmc_spi_internal.h stays: tests/storage/src/test_ra8_sdmmc_spi_cov.c
# and tests/support/inc/sdmmc_spi_cov_test_util.h include it, so the
# libs/ra8_sdmmc_spi/src include dirs stay too.
ra8_add_zig_library(
  NAME
  ra8_sdmmc_spi
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_sdmmc_spi
  LIBRARY_NAME
  ra8_sdmmc_spi
)

# The BACKEND-AGNOSTIC HALF only: the dispatcher behind inc/ra8_display_pal.h
# (argument guards, the one module-static handle, dispatch through the bound
# vtable) and the page-turn refresh cadence behind
# inc/ra8_display_pal_policy.h are Zig now, so src/ra8_display_pal.c and
# src/ra8_display_pal_policy.c are gone. Both panel backends followed: the
# LCD/GLCDC one is src/ra8_display_pal_lcd_abi.zig over src/internal/lcd.zig,
# and the IT8951 e-paper one is src/ra8_display_pal_eink_abi.zig over
# src/internal/eink.zig. They still drive ra8_glcdc and ra8_epaper inside
# ra8_core_hal, through extern declarations rather than a C include. Nothing
# under libs/ra8_display_pal/src is C any more, so RA8_DISPLAY_PAL_SOURCES is
# empty. The panel stays a caller-supplied vtable,
# so the archive names no controller and the host suite's fake backends
# substitute for one exactly as before. src/ra8_display_pal_internal.h stays
# too: tests/graphics/src/test_ra8_display_pal.c includes it, so the
# libs/ra8_display_pal/src include dirs stay in core_hal.cmake and
# unit_tests.cmake.
ra8_add_zig_library(
  NAME
  ra8_display_pal
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_display_pal
  LIBRARY_NAME
  ra8_display_pal
)

# Fully migrated: the parsing/validation cluster, the orchestration state
# machine (which owns g_ra8_ota_cfg, g_ra8_ota_state, g_ra8_ota_initialized and
# g_ra8_ota_buf) and the verify cluster are all this archive, so
# libs/ra8_ota/src carries no C at all. The weak ra8_ota_system_reset_hook
# default is a separate archive member, so a strong definition in the image (or
# in tests/misc/src/test_ra8_ota.c) still overrides it. src/ra8_ota_internal.h
# stays -- three C suites include it -- so the libs/ra8_ota/src include dirs
# stay in core_hal.cmake and unit_tests.cmake.
ra8_add_zig_library(
  NAME
  ra8_ota
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_ota
  LIBRARY_NAME
  ra8_ota
)

target_link_libraries(
  ra8_core_hal
  PUBLIC ra8_zig::ra8_box
         ra8_zig::ra8_power_profile
         ra8_zig::ra8_epd_cal
         ra8_zig::ra8_touch_cal
         ra8_zig::ra8_devcfg
         ra8_zig::ra8_batt
         ra8_zig::ra8_ui
         ra8_zig::ra8_app
         ra8_zig::ra8_wdt_supervisor
         ra8_zig::ra8_mpu
         ra8_zig::ra8_net_pal
         ra8_zig::ra8_lsm6dso
         ra8_zig::ra8_usb_pal
         ra8_zig::ra8_keyboard
         ra8_zig::ra8_audio
         ra8_zig::ra8_wifi
         ra8_zig::ra8_ov5640
         ra8_zig::if_ra8_vfs
         ra8_zig::ra8_camera
         ra8_zig::fw_if_fs
         ra8_zig::ra8_ftl
         ra8_zig::ra8_sdmmc_spi
         ra8_zig::ra8_display_pal
         ra8_zig::ra8_ota
  ra8_zig::ra8_gfx
         ra8_zig::ra8_gfx
)

link_libraries(ra8_zig::ra8_batt)

# Object-library consumers do not inherit the core target's link interface;
# attach migrated archives at directory scope so every host test links them.
link_libraries(
  ra8_zig::ra8_box
  ra8_zig::ra8_power_profile
  ra8_zig::ra8_epd_cal
  ra8_zig::ra8_touch_cal
  ra8_zig::ra8_devcfg
  ra8_zig::ra8_ui
  ra8_zig::ra8_app
  ra8_zig::ra8_wdt_supervisor
  ra8_zig::ra8_mpu
  ra8_zig::ra8_net_pal
  ra8_zig::ra8_lsm6dso
  ra8_zig::ra8_usb_pal
  ra8_zig::ra8_keyboard
  ra8_zig::ra8_audio
  ra8_zig::ra8_wifi
  ra8_zig::ra8_ov5640
  ra8_zig::if_ra8_vfs
  ra8_zig::ra8_camera
  ra8_zig::fw_if_fs
  ra8_zig::ra8_ftl
  ra8_zig::ra8_sdmmc_spi
  ra8_zig::ra8_display_pal
  ra8_zig::ra8_ota
)

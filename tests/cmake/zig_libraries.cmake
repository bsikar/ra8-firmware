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
# no .c left (the host ThreadX shim header went with it) and the
# RA8_WDT_SUPERVISOR_SOURCES glob is gone from library_sources.cmake and
# core_hal.cmake.
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

# Fully migrated: the per-endpoint packet rings, the two MC/DC-promoted
# predicates, the ra8_usb status -> PAL event translation and the whole public
# surface are Zig now, so libs/ra8_usb_pal/src has no .c left and the
# RA8_USB_PAL_SOURCES glob is gone from library_sources.cmake and
# core_hal.cmake. src/ra8_usb_pal_internal.h stays: the host suite includes it
# to drive the two promoted predicates, which the Zig archive exports under the
# same names. The Ring-3 ra8_usb driver stays a link-time seam.
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
set_property(
  TARGET ra8_zig::ra8_keyboard
  APPEND
  PROPERTY INTERFACE_LINK_LIBRARIES ra8_zig::ra8_ui
)

# ra8_core_hal is the OBJECT library every host test links, so an INTERFACE
# link here reaches each test executable that pulls in a migrated library.
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
)

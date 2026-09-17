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
)

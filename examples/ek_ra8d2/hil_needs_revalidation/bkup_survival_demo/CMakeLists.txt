#
# Per-app CMake stub for `bkup_survival_demo`. All build logic lives in the shared
# cmake/ra8_add_app.cmake recipe; this file only names the app.
#
# Standalone:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-ra8d2.cmake
# Embedded:    pulled in by the top-level CMakeLists via add_subdirectory().
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
cmake_minimum_required(VERSION 3.20)

# A standalone `cmake -S . -B build` makes this file the top-level
# listfile, which CMake requires to contain a literal, direct project()
# call. The aggregate repo-root build instead pulls this app in via
# add_subdirectory() (project() is already called at the root), so guard on
# PARENT_DIRECTORY to avoid a redundant nested sub-project. ra8_add_app()
# below re-runs project() in the standalone case to apply the shared VERSION
# and the app description.
get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(bkup_survival_demo LANGUAGES C ASM)
endif()

# Find the repo root from this app dir, depth-independent.
set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
  get_filename_component(_d "${_d}" DIRECTORY)
endwhile()
include("${_d}/cmake/ra8_add_app.cmake")

ra8_add_app(
  NAME bkup_survival_demo
  STACK_BYTES 2200
  LIBS ra8_board_ek_ra8d2
  DESCRIPTION "bkup_survival_demo firmware for RA8D2"
)

# Issue #131: the VBATT battery-backup function is inoperative unless voltage
# monitor 0 (LVD0) reset is enabled. HUM Ch 12.1.3 p 499: "It is necessary to
# enable voltage monitor 0 resets to use the battery backup function"; HUM
# Ch 12.3.2 p 514: "The battery backup function should be used after the
# voltage monitoring 0 reset is enabled (OFS1.PVDAS bit is 0)". With the
# permissive default OFS1 = 0xFFFFFFFF, PVDAS = 1 (LVD0 disabled), so the
# VBATT-backed register area stays held in VBATT_POR reset (VBPORF = 1) and
# every VBTBKRn write is silently dropped on silicon (rw=BAD) -- ra8_emulator
# cannot model this, which is why the fake passed while the bench failed.
# Enable LVD0 (PVDAS = 0, bit 3) with VDSEL0 = 000 = 2.85 V (HUM Ch 7 p 284):
# below VCC 3.3 V so it never trips a spurious reset, and above the highest
# 2.80 V VDETBATT switch level (HUM Ch 12.3.2 requires LVD0 > switch level).
# OFS1 = 0xFFFFFFFF with bits 3:0 cleared -> 0xFFFFFFF0. OFS1_SEC matches so
# the setting applies whichever OFS1_SEL selects.
target_compile_definitions(
  bkup_survival_demo.elf PRIVATE BSP_CFG_OPTION_SETTING_OFS1=0xFFFFFFF0U
                                 BSP_CFG_OPTION_SETTING_OFS1_SEC=0xFFFFFFF0U
)

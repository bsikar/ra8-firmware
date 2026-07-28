# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Storage tests that link a standalone subset rather than ra8_core_hal.
#
# exFAT links ONLY ra8_fs_fat.c (no ra8_core_hal means no ra8_time weak-extern,
# so it builds on macOS and Linux alike); the cache-store pair builds its own
# sources plus the vendored LevelX NOR driver. Each also owns a fixture or a
# custom command, which is why none of them can come from the auto-glob.
#
# Included from tests/CMakeLists.txt. CMake include() is textual within the
# same directory scope, so every variable and target defined here is visible
# to the driver and to the fragments included after it.

# ---------------------------------------------------------------------------
# exFAT host test (#85 read support / #93 leading-slash open regression).
# Standalone: links ONLY ra8_fs_fat.c (no ra8_core_hal -> no ra8_time
# weak-extern), so it builds and runs on macOS and Linux alike. The fixture
# is a tiny real exFAT image checked in gzipped; it is decompressed to the
# build dir at build time and its path injected via RA8_EXFAT_FIXTURE.
# ---------------------------------------------------------------------------
set(RA8_EXFAT_FIXTURE_GZ ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ra8_fs/exfat_small.img.gz)
set(RA8_EXFAT_FIXTURE_IMG ${CMAKE_CURRENT_BINARY_DIR}/exfat_small.img)
add_custom_command(
  OUTPUT ${RA8_EXFAT_FIXTURE_IMG}
  COMMAND bash -c "gunzip -c '${RA8_EXFAT_FIXTURE_GZ}' > '${RA8_EXFAT_FIXTURE_IMG}'"
  DEPENDS ${RA8_EXFAT_FIXTURE_GZ}
  COMMENT "Decompressing exFAT test fixture"
  VERBATIM
)
add_custom_target(
  ra8_exfat_fixture
  DEPENDS ${RA8_EXFAT_FIXTURE_IMG}
  COMMENT "Materialising the exFAT test fixture image"
)
# ra8_fs_fat.c was split into ra8_fs_fat*.c TUs for the 1000-line file-size cap;
# glob them all so this standalone test still links the whole FAT driver (and
# only the FAT driver -- no ra8_core_hal -> no ra8_time weak-extern).
file(GLOB RA8_FS_FAT_TU_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_fs/src/ra8_fs_fat*.c)
add_executable(
  test_ra8_fs_exfat ${CMAKE_CURRENT_SOURCE_DIR}/host/exfat_fs_test.c ${RA8_FS_FAT_TU_SOURCES}
)
add_dependencies(test_ra8_fs_exfat ra8_exfat_fixture)
target_compile_options(test_ra8_fs_exfat PRIVATE -Wall -Wextra)
target_include_directories(
  test_ra8_fs_exfat PRIVATE ${FW_ROOT}/libs/ra8_fs/inc ${FW_ROOT}/libs/ra8_core/inc
                            ${FW_ROOT}/libs/ra8_hal/inc
)
target_compile_definitions(test_ra8_fs_exfat PRIVATE RA8_EXFAT_FIXTURE="${RA8_EXFAT_FIXTURE_IMG}")
add_test(NAME test_ra8_fs_exfat COMMAND test_ra8_fs_exfat)

# ---------------------------------------------------------------------------
# test_ra8_cache_store (#201): persistent key->blob cache over LevelX standalone.
# Builds ra8_cache_store's own sources plus the vendored LevelX NOR sources
# (LX_STANDALONE_ENABLE, no ThreadX) and the RAM NOR simulator mock. ra8_core_hal
# supplies ra8_log / ra8_err / ra8_vsource. LevelX is SOUP: compile it -w and it is
# already outside the coverage filter (libs/third_party/).
# ---------------------------------------------------------------------------
file(GLOB RA8_CACHE_STORE_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra8_cache_store/src/*.c)
file(GLOB RA8_LEVELX_NOR_STANDALONE CONFIGURE_DEPENDS
     ${FW_ROOT}/libs/third_party/levelx/common/src/lx_nor_*.c
)
# Drop the upstream NOR simulator: this test supplies its own RAM NOR driver.
list(
  FILTER
  RA8_LEVELX_NOR_STANDALONE
  EXCLUDE
  REGEX
  ".*/lx_nor_flash_simulator\\.c$"
)
add_executable(
  test_ra8_cache_store
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_cache_store.c
  ${RA8_CACHE_STORE_SOURCES}
  ${RA8_LEVELX_NOR_STANDALONE}
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/lx_nor_sim_ram.c
  $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_cache_store PROPERTIES LINKER_LANGUAGE CXX)
target_compile_definitions(test_ra8_cache_store PRIVATE LX_STANDALONE_ENABLE)
target_compile_options(
  test_ra8_cache_store PRIVATE -Wall -Wextra -Wno-unused-function -Wno-unused-parameter
)
target_include_directories(
  test_ra8_cache_store
  PRIVATE ${FW_ROOT}/libs/ra8_cache_store/inc
          ${FW_ROOT}/libs/ra8_cache_store/src
          ${FW_ROOT}/libs/ra8_core/inc
          ${FW_ROOT}/libs/ra8_hal/inc
          ${FW_ROOT}/libs/ra8_mem/inc
          ${FW_ROOT}/libs/third_party/levelx/common/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
# LevelX SOUP: silence its warnings (matches cmake/levelx.cmake handling).
set_source_files_properties(${RA8_LEVELX_NOR_STANDALONE} PROPERTIES COMPILE_OPTIONS "-w")
add_test(NAME test_ra8_cache_store COMMAND test_ra8_cache_store)

# ---------------------------------------------------------------------------
# test_cache_store_demo (#257): the ra8_cache_store_demo example core on the host.
# Compiles the SAME demo core (cache_store_demo.c) and RAM NOR driver
# (lx_nor_ram.c) the ARM example runs, so the host test and the ra8_emulator gate
# exercise byte-identical logic. Reuses the cache_store + LevelX-standalone
# sources globbed for test_ra8_cache_store above; ra8_core_hal supplies ra8_log /
# ra8_err / ra8_check. The example sources live under examples/ (outside the
# coverage filter), so they add no per-file coverage-floor obligation.
# ---------------------------------------------------------------------------
set(RCS_DEMO_DIR ${FW_ROOT}/examples/ek_ra8d2/hil_needs_revalidation/ra8_cache_store_demo)
add_executable(
  test_cache_store_demo
  ${CMAKE_CURRENT_SOURCE_DIR}/test_cache_store_demo.c
  ${RCS_DEMO_DIR}/src/cache_store_demo.c
  ${RCS_DEMO_DIR}/src/lx_nor_ram.c
  ${RA8_CACHE_STORE_SOURCES}
  ${RA8_LEVELX_NOR_STANDALONE}
  $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_cache_store_demo PROPERTIES LINKER_LANGUAGE CXX)
target_compile_definitions(test_cache_store_demo PRIVATE LX_STANDALONE_ENABLE)
target_compile_options(
  test_cache_store_demo PRIVATE -Wall -Wextra -Wno-unused-function -Wno-unused-parameter
)
target_include_directories(
  test_cache_store_demo
  PRIVATE # Cache-store demo specifics: the example core, the cache-store library
          # (public + private for MC/DC access) and the standalone LevelX headers.
          ${RCS_DEMO_DIR}/inc
          ${FW_ROOT}/libs/ra8_cache_store/inc
          ${FW_ROOT}/libs/ra8_cache_store/src
          ${FW_ROOT}/libs/third_party/levelx/common/inc
          # This is the FIRST test to pull examples/ .c sources (cache_store_demo.c,
          # lx_nor_ram.c) into the tidy compile_commands.json. clang-tidy lints every
          # examples/<app> dir; an app not in the DB has its command INTERPOLATED from
          # the nearest DB entry, and these examples/ entries become that neighbour for
          # the other example apps. Give them the same broad first-party include set
          # ra8_add_test() uses so an interpolated app (audio_loopback -> ra8_board...,
          # threadx_nimble_peripheral -> port/nimble, ...) still resolves its headers
          # instead of a clang-diagnostic-error. The extra dirs are harmless to the
          # actual test build.
          ${FW_ROOT}/libs/ra8_core/inc
          ${FW_ROOT}/libs/ra8_hal/inc
          ${FW_ROOT}/libs/ra8_net_pal/inc
          ${FW_ROOT}/libs/ra8_modem_at/inc
          ${FW_ROOT}/libs/ra8_usb_pal/inc
          ${FW_ROOT}/libs/ra8_fs/inc
          ${FW_ROOT}/libs/ra8_io/inc
          ${FW_ROOT}/libs/ra8_ftl/inc
          ${FW_ROOT}/libs/ra8_mem/inc
          ${FW_ROOT}/libs/ra8_sdmmc_spi/inc
          ${FW_ROOT}/libs/ra8_sdfont/inc
          ${FW_ROOT}/libs/ra8_gfx/inc
          ${FW_ROOT}/libs/ra8_ui/inc
          ${FW_ROOT}/libs/ra8_keyboard/inc
          ${FW_ROOT}/libs/ra8_box/inc
          ${FW_ROOT}/libs/ra8_book/inc
          ${FW_ROOT}/libs/ra8_rabook_compile/inc
          ${FW_ROOT}/libs/ra8_rabook_import/inc
          ${FW_ROOT}/libs/ra8_batt/inc
          ${FW_ROOT}/libs/ra8_widget/inc
          ${FW_ROOT}/libs/ra8_app/inc
          ${FW_ROOT}/libs/ra8_tls/inc
          ${FW_ROOT}/libs/ra8_nsc/inc
          ${FW_ROOT}/libs/ra8_ota/inc
          ${FW_ROOT}/libs/ra8_dfu/inc
          ${FW_ROOT}/libs/ra8_display_pal/inc
          ${FW_ROOT}/libs/ra8_power_profile/inc
          ${FW_ROOT}/libs/ra8_epub/inc
          ${FW_ROOT}/libs/ra8_comic/inc
          ${FW_ROOT}/libs/ra8_unarch/inc
          ${FW_ROOT}/libs/ra8_reflow/inc
          ${FW_ROOT}/libs/ra8_webp/inc
          ${FW_ROOT}/libs/ra8_touch_cal/inc
          ${FW_ROOT}/libs/ra8_epd_cal/inc
          ${FW_ROOT}/libs/ra8_mpu/inc
          ${FW_ROOT}/libs/ra8_psa_crypto/inc
          ${FW_ROOT}/libs/ra8_wdt_supervisor/inc
          ${FW_ROOT}/libs/ra8_board_ek_ra8d2/inc
          ${FW_ROOT}/libs/ra8_lsm6dso/inc
          ${FW_ROOT}/libs/ra8_tz_secure_boot/inc
          ${FW_ROOT}/port/threadx/inc
          ${FW_ROOT}/libs/third_party/miniz
          ${FW_ROOT}/libs/third_party/tinyxml2
          ${FW_ROOT}/libs/third_party/stb
          ${FW_ROOT}/src/secure_app/inc
)
add_test(NAME test_cache_store_demo COMMAND test_cache_store_demo)

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# ra8_add_test(), and the auto-glob that registers one executable per
# tests/src/test_*.c.
#
# Everything registered here builds with the standard host profile. A test
# needing anything else -- a device gate, a real crypto backend, C++, or a
# standalone library subset -- is defined in one of the tests_*.cmake
# fragments and REMOVE_ITEM-ed from the glob above.
#
# Included from tests/CMakeLists.txt. CMake include() is textual within the
# same directory scope, so every variable and target defined here is visible
# to the driver and to the fragments included after it.

# Enable ctest so `cmake --build . --target test` and bash helpers work.
enable_testing()

# ---------------------------------------------------------------------------
# Unit tests -- one executable per test_*.c file. Each test links
# against the ra8_core_hal OBJECT library and runs standalone.
# ---------------------------------------------------------------------------

# Declare one host unit-test executable.
#
# Builds <name> from <name>.c linked against the ra8_core_hal OBJECT
# library, applies the host warning profile, and registers it with ctest.
#
#   name                       test basename, without the .c suffix
set(RA8_TEST_SHARED_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/inc ${CMAKE_CURRENT_SOURCE_DIR}/mocks/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/support/inc
)

# ra8_add_test: create and register one standalone host test executable.
function(ra8_add_test name src_file)
  get_filename_component(_ra8_test_src_dir "${src_file}" DIRECTORY)
  get_filename_component(_ra8_test_src_leaf "${_ra8_test_src_dir}" NAME)
  get_filename_component(_ra8_test_unit_dir "${_ra8_test_src_dir}" DIRECTORY)
  set(_ra8_test_include_dirs ${RA8_TEST_SHARED_INCLUDE_DIRS})
  if(_ra8_test_src_leaf STREQUAL "src" AND IS_DIRECTORY "${_ra8_test_unit_dir}/inc")
    list(APPEND _ra8_test_include_dirs "${_ra8_test_unit_dir}/inc")
  endif()

  if(name STREQUAL "test_comic_cbt")
    list(APPEND _ra8_test_include_dirs ${FW_ROOT}/apps/shared_libs/unarch/tests/inc)
  elseif(name STREQUAL "test_rabook_import_streamed" OR name STREQUAL "test_rabook_import_m33")
    list(APPEND _ra8_test_include_dirs ${FW_ROOT}/apps/shared_libs/rabook_compile/tests/inc)
  elseif(name STREQUAL "test_ra8_mkbookimg_names")
    list(APPEND _ra8_test_include_dirs ${FW_ROOT}/tools/mkbookimg/inc)
  elseif(name STREQUAL "test_ereader_pageturn")
    list(APPEND _ra8_test_include_dirs ${FW_ROOT}/examples/ek_ra8d2/hw_validated/hil/ereader_ui/inc)
  elseif(name STREQUAL "test_ra8_shelf_classify" OR name STREQUAL "test_app_ereader_shelf_comic")
    list(APPEND _ra8_test_include_dirs
         ${FW_ROOT}/examples/ek_ra8d2/hil_needs_revalidation/ereader_shelf/inc
    )
  elseif(name STREQUAL "test_app_ereader_manga")
    list(APPEND _ra8_test_include_dirs ${FW_ROOT}/examples/ek_ra8d2/hw_pending/ereader_manga/inc)
  elseif(name STREQUAL "test_app_usb_printer_vendor")
    list(APPEND _ra8_test_include_dirs
         ${FW_ROOT}/examples/ek_ra8d2/hw_pending/manual/usb_printer_vendor/inc
    )
  endif()

  add_executable(${name} ${src_file} $<TARGET_OBJECTS:ra8_core_hal>)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(${name} PRIVATE RA8_TEST_REPO_ROOT="${FW_ROOT}")
  set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)
  if(name STREQUAL "test_ra8_nsc_periph_init_cov")
    target_link_options(
      ${name}
      PRIVATE
      -Wl,--wrap=ra8_pwr_init
      -Wl,--wrap=ra8_isr_init
      -Wl,--wrap=ra8_dma_init
    )
  endif()

  target_include_directories(
    ${name}
    PRIVATE ${_ra8_test_include_dirs}
            ${FW_ROOT}/libs/ra8_core/inc
            ${FW_ROOT}/apps/shared_libs/xml/inc
            ${FW_ROOT}/libs/ra8_hal/inc
            ${FW_ROOT}/libs/ra8_jpeg/inc
            ${FW_ROOT}/libs/ra8_net_pal/inc
            ${FW_ROOT}/libs/ra8_modem_at/inc
            ${FW_ROOT}/libs/ra8_usb_pal/inc
            ${FW_ROOT}/libs/ra8_fs/inc
            ${FW_ROOT}/libs/ra8_io/inc
            ${FW_ROOT}/apps/shared_libs/compress/inc
            ${FW_ROOT}/libs/ra8_audio/inc
            ${FW_ROOT}/libs/ra8_camera/inc
            ${FW_ROOT}/libs/ra8_camera_io/inc
            ${FW_ROOT}/libs/ra8_ftl/inc
            ${FW_ROOT}/libs/ra8_mem/inc
            ${FW_ROOT}/libs/ra8_sdmmc_spi/inc
            ${FW_ROOT}/libs/ra8_sdfont/inc
            ${FW_ROOT}/libs/ra8_gfx/inc
            ${FW_ROOT}/libs/ra8_ui/inc
            ${FW_ROOT}/libs/ra8_keyboard/inc
            ${FW_ROOT}/libs/ra8_box/inc
            ${FW_ROOT}/apps/shared_libs/book/inc
            ${FW_ROOT}/apps/shared_libs/rabook_compile/inc
            ${FW_ROOT}/apps/shared_libs/rabook_import/inc
            ${FW_ROOT}/libs/ra8_batt/inc
            ${FW_ROOT}/libs/ra8_widget/inc
            ${FW_ROOT}/libs/ra8_app/inc
            ${FW_ROOT}/libs/ra8_tls/inc
            ${FW_ROOT}/libs/ra8_nsc/inc
            ${FW_ROOT}/libs/ra8_ota/inc
            ${FW_ROOT}/libs/ra8_dfu/inc
            ${FW_ROOT}/libs/ra8_devcfg/inc
            ${FW_ROOT}/libs/ra8_wifi/inc
            ${FW_ROOT}/libs/ra8_display_pal/inc
            ${FW_ROOT}/libs/ra8_power_profile/inc
            ${FW_ROOT}/apps/shared_libs/epub/inc
            ${FW_ROOT}/apps/shared_libs/comic/inc
            ${FW_ROOT}/apps/shared_libs/unarch/inc
            ${FW_ROOT}/apps/shared_libs/jof/inc
            ${FW_ROOT}/apps/shared_libs/longstrip/inc
            ${FW_ROOT}/apps/shared_libs/zoom/inc
            ${FW_ROOT}/apps/shared_libs/reflow/inc
            ${FW_ROOT}/apps/shared_libs/webp/inc
            ${FW_ROOT}/libs/ra8_touch_cal/inc
            ${FW_ROOT}/libs/ra8_epd_cal/inc
            ${FW_ROOT}/libs/ra8_mpu/inc
            ${FW_ROOT}/libs/ra8_psa_crypto/inc
            ${FW_ROOT}/libs/ra8_wdt_supervisor/inc
            ${FW_ROOT}/libs/ra8_board_ek_ra8d2/inc
            ${FW_ROOT}/libs/ra8_lsm6dso/inc
            ${FW_ROOT}/libs/ra8_ov5640/inc
            ${FW_ROOT}/libs/ra8_tz_secure_boot/inc
            ${FW_ROOT}/port/threadx/inc
            ${FW_ROOT}/port/esp-hosted/inc
            ${FW_ROOT}/port/esp-hosted/inc/idf_compat
            ${FW_ROOT}/port/esp-hosted/src
            ${FW_ROOT}/port/esp-hosted/tests/inc
            ${FW_ROOT}/libs/third_party/esp-hosted/host
            ${FW_ROOT}/libs/third_party/esp-hosted/host/api/include
            ${FW_ROOT}/libs/third_party/esp-hosted/host/drivers/transport
            ${FW_ROOT}/libs/third_party/esp-hosted/host/drivers/transport/spi
            ${FW_ROOT}/libs/third_party/esp-hosted/common
            ${FW_ROOT}/libs/third_party/esp-hosted/common/log
            ${FW_ROOT}/libs/third_party/esp-hosted/common/mempool/include
            ${FW_ROOT}/libs/third_party/esp-hosted/common/transport
            ${FW_ROOT}/apps/shared_libs/third_party/miniz
            ${FW_ROOT}/apps/shared_libs/third_party/stb
            ${FW_ROOT}/apps/shared_libs/third_party/xz_embedded
            ${FW_ROOT}/libs/ra8_secure_app/inc
            # Per-module src/ for MC/DC test access (see CLAUDE.md).
            ${FW_ROOT}/libs/ra8_core/src
            ${FW_ROOT}/libs/ra8_hal/src
            ${FW_ROOT}/libs/ra8_secure_app/src
            ${FW_ROOT}/libs/ra8_jpeg/src
            ${FW_ROOT}/libs/ra8_net_pal/src
            ${FW_ROOT}/libs/ra8_modem_at/src
            ${FW_ROOT}/libs/ra8_tls/src
            ${FW_ROOT}/libs/ra8_usb_pal/src
            ${FW_ROOT}/libs/ra8_fs/src
            ${FW_ROOT}/libs/ra8_io/src
            ${FW_ROOT}/libs/ra8_audio/src
            ${FW_ROOT}/libs/ra8_camera/src
            ${FW_ROOT}/libs/ra8_camera_io/src
            ${FW_ROOT}/libs/ra8_ftl/src
            ${FW_ROOT}/libs/ra8_sdmmc_spi/src
            ${FW_ROOT}/libs/ra8_gfx/src
            ${FW_ROOT}/libs/ra8_nsc/src
            ${FW_ROOT}/libs/ra8_ota/src
            ${FW_ROOT}/libs/ra8_dfu/src
            ${FW_ROOT}/libs/ra8_devcfg/src
            ${FW_ROOT}/libs/ra8_wifi/src
            ${FW_ROOT}/libs/ra8_display_pal/src
            ${FW_ROOT}/libs/ra8_power_profile/src
            ${FW_ROOT}/libs/ra8_touch_cal/src
            ${FW_ROOT}/libs/ra8_epd_cal/src
            ${FW_ROOT}/libs/ra8_mpu/src
            ${FW_ROOT}/libs/ra8_psa_crypto/src
            ${FW_ROOT}/libs/ra8_wdt_supervisor/src
            ${FW_ROOT}/libs/ra8_board_ek_ra8d2/src
            ${FW_ROOT}/libs/ra8_lsm6dso/src
            ${FW_ROOT}/libs/ra8_ov5640/src
            ${FW_ROOT}/apps/shared_libs/epub/src
            ${FW_ROOT}/apps/shared_libs/comic/src
            ${FW_ROOT}/apps/shared_libs/unarch/src
            ${FW_ROOT}/apps/shared_libs/jof/src
            ${FW_ROOT}/apps/shared_libs/longstrip/src
            ${FW_ROOT}/apps/shared_libs/zoom/src
            ${FW_ROOT}/apps/shared_libs/reflow/src
            ${FW_ROOT}/libs/ra8_tz_secure_boot/src
  )
  if(REFLOW_USE_LITEHTML)
    # ra8_core_hal is an OBJECT library, so link deps don't propagate
    # automatically; bind litehtml/gumbo directly into each test exe
    # so the v2 adapter's symbols resolve.
    target_link_libraries(${name} PRIVATE litehtml gumbo)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# Auto-discover every test_*.c file in each category's src/ directory and register it
# via ra8_add_test(). Dropping a new test_foo.c file is enough -- no
# manual list edit required. (CONFIGURE_DEPENDS means CMake re-globs
# on the next build if the directory contents changed.)
file(GLOB RA8_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*/src/test_*.c)

# v1's test_reflow.c and its split sibling test_reflow_api_mcdc.c
# assert on glyph-array internals that v2 deliberately does not populate
# (shared fixture: apps/shared_libs/reflow/tests/inc/reflow_v1_test_util.h); drop them under
# v2 and add the C++ v2 test below in their place.
if(REFLOW_USE_LITEHTML)
  list(REMOVE_ITEM RA8_TEST_SOURCES ${FW_ROOT}/apps/shared_libs/reflow/tests/src/test_reflow.c)
  list(REMOVE_ITEM RA8_TEST_SOURCES
       ${FW_ROOT}/apps/shared_libs/reflow/tests/src/test_reflow_api_mcdc.c
  )
endif()

# test_coverage_compile_all.c is the coverage aggregator: a single
# no-op TU whose only purpose is to force every first-party source
# to be linked with the active coverage instrumentation flags so
# llvm-cov / gcovr can compute uniform repository-wide coverage.
# It contributes zero functional test value, so we exclude it from
# the fast `just quality::local::test` flow (RA8_MCDC=OFF) and only build it when
# `RA8_MCDC=ON`.
if(NOT RA8_MCDC)
  list(REMOVE_ITEM RA8_TEST_SOURCES
       ${CMAKE_CURRENT_SOURCE_DIR}/misc/src/test_coverage_compile_all.c
  )
endif()

# test_psa_real_kat.c links the REAL crypto backend (TF-PSA-Crypto), not the
# off-target ra8_core_hal, so it is registered by hand below rather than through the
# ra8_add_test() auto-glob (which would splice in the RA8_OFF_TARGET build).
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/misc/src/test_psa_real_kat.c)

# test_ra8_cache_store.c needs the vendored LevelX NOR sources + a RAM NOR
# fake + LX_STANDALONE_ENABLE, so it is registered by hand below rather
# than through the ra8_add_test() auto-glob.
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/misc/src/test_ra8_cache_store.c)

# test_lx_fs_backend.c (#611) compiles the vendored LevelX NOR sources + the
# RAM NOR fake + the port/levelx ra8_fs backend shim with LX_STANDALONE_ENABLE,
# so it is registered by hand (tests/cmake/tests_storage.cmake) rather than
# through the ra8_add_test() auto-glob.
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/misc/src/test_lx_fs_backend.c)

# test_cache_store_demo.c (issue #257) compiles the ra8_cache_store_demo example
# core + RAM NOR driver from examples/ plus the vendored LevelX NOR sources with
# LX_STANDALONE_ENABLE, so it is registered by hand below rather than through the
# ra8_add_test() auto-glob.
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/misc/src/test_cache_store_demo.c)

# test_ra8_rsip_devsec_failclosed.c (issue #216) must compile ra8_rsip_devsec.c
# with the stub-crypto guard flags UNDEFINED so the production fail-closed #else
# is the body under test. The rest of the host build force-defines
# RA8_OFF_TARGET, so it is registered by hand below rather than through the
# ra8_add_test() auto-glob (which would splice in the RA8_OFF_TARGET build).
list(REMOVE_ITEM RA8_TEST_SOURCES
     ${CMAKE_CURRENT_SOURCE_DIR}/security/src/test_ra8_rsip_devsec_failclosed.c
)

# test_mbedtls_psa_rng.c compiles the HTTPS example's production RNG adapter
# with RA8_OFF_TARGET undefined and a test-scoped PSA ABI header. Register it by
# hand in tests_crypto.cmake so the shared off-target object library cannot hide
# the production callback body.
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/security/src/test_mbedtls_psa_rng.c)

# test_ra8_npu.c (issue #221) drives the Arm Ethos-U55 NPU driver, whose body in
# ra8_npu.c is device-gated behind RA8_HAS_NPU (RA8P1-only). The shared ra8_core_hal
# object library is compiled for the default RA8D2, so its ra8_npu.c is an EMPTY
# TU with no NPU symbols. It is registered by hand below with -DRA8_DEVICE_RA8P1
# (which makes ra8_device.h define RA8_HAS_NPU) so a live ra8_npu.c is compiled and
# linked; the auto-glob would build it for the RA8D2 and fail to resolve the NPU
# API. This is the same "compile one TU under a different device profile" pattern
# as test_ra8_rsip_devsec_failclosed above.
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/misc/src/test_ra8_npu.c)

# test_ra8_ethosu_shim.c (issue #228) drives the Arm ethos-u-core-driver -> ra8_npu
# adapter (ra8_ethosu_shim.c), whose body -- like ra8_npu.c -- is device-gated behind
# RA8_HAS_NPU (RA8P1-only). The shared ra8_core_hal object library is compiled for the
# default RA8D2, so its ra8_ethosu_shim.c / ra8_npu.c are EMPTY TUs. It is registered by
# hand below with -DRA8_DEVICE_RA8P1 (same pattern as test_ra8_npu) so live adapter +
# driver bodies are compiled and linked; the auto-glob would build it for the RA8D2 and
# fail to resolve the ethosu_* / ra8_npu_* API.
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/misc/src/test_ra8_ethosu_shim.c)

# test_ra8_npu_loader.c (issue #227) drives the .npub Vela-blob loader
# (ra8_npu_loader.c), which -- like ra8_npu.c -- is device-gated behind
# RA8_HAS_NPU (RA8P1-only) and turns a committed golden model container
# (tools/vela/generated/ra8_npu_model_addk_fake.h) into an ra8_npu_job_t. It is
# registered by hand below with -DRA8_DEVICE_RA8P1 (same pattern as test_ra8_npu)
# so the live loader + driver bodies compile and link; the auto-glob would build
# it for the RA8D2 and fail to resolve the ra8_npu_* API.
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/misc/src/test_ra8_npu_loader.c)

# test_ra8_emulator_mstp_gate.c (#405) compiles the engine-free ra8_emulator MSTP
# model (tools/ra8_emulator/src/periph/board_periph_mstp_model.c) alongside it to exercise
# the address->module-stop-bit gate table directly, so it is registered by hand
# in tests_ra8_emulator.cmake rather than through the ra8_add_test() auto-glob
# (which would build it against ra8_core_hal without the model source).
list(REMOVE_ITEM RA8_TEST_SOURCES
     ${CMAKE_CURRENT_SOURCE_DIR}/misc/src/test_ra8_emulator_mstp_gate.c
)

# test_ra8_c6link.c / test_ra8_c6link_wire.c (#490) drive libs/ra8_c6link, which
# speaks the vendored esp-hosted protobuf wire format. They therefore need the
# generated codec + the protobuf-c runtime compiled alongside them and the
# esp-hosted include path, neither of which ra8_core_hal carries, so both are
# registered by hand in tests_c6link.cmake rather than through the auto-glob.
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_c6link.c)
list(REMOVE_ITEM RA8_TEST_SOURCES
     ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_c6link_session.c
)
list(REMOVE_ITEM RA8_TEST_SOURCES
     ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_c6link_transport.c
)
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_c6link_media.c)
list(REMOVE_ITEM RA8_TEST_SOURCES
     ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_c6link_media_decoder.c
)
list(REMOVE_ITEM RA8_TEST_SOURCES
     ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_c6link_media_http.c
)
list(REMOVE_ITEM RA8_TEST_SOURCES
     ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_c6link_transfer_coordinator.c
)
list(REMOVE_ITEM RA8_TEST_SOURCES
     ${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_ra8_c6link_mdl_decode.c
)
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_c6link_wire.c)
list(REMOVE_ITEM RA8_TEST_SOURCES ${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_ra8_c6link_mdl.c)
list(REMOVE_ITEM RA8_TEST_SOURCES
     ${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_ra8_c6link_mdl_policy.c
)
list(REMOVE_ITEM RA8_TEST_SOURCES
     ${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_ra8_c6link_mdl_codec.c
)
list(REMOVE_ITEM RA8_TEST_SOURCES
     ${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_ra8_c6link_mdl_guards.c
)
list(REMOVE_ITEM RA8_TEST_SOURCES
     ${FW_ROOT}/apps/shared_libs/rabook_compile/tests/src/test_ra8_c6link_rabook.c
)
list(REMOVE_ITEM RA8_TEST_SOURCES
     ${CMAKE_CURRENT_SOURCE_DIR}/mocks/src/test_app_media_download_format.c
)
list(REMOVE_ITEM RA8_TEST_SOURCES
     ${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_mdl_net_c6link_mock.c
)
list(REMOVE_ITEM RA8_TEST_SOURCES
     ${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_ra8_esp32_c6_mdl_service.c
)
list(REMOVE_ITEM RA8_TEST_SOURCES ${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_mdl_storage_ram.c)
list(REMOVE_ITEM RA8_TEST_SOURCES
     ${FW_ROOT}/apps/shared_libs/mdl_storage_vfs/tests/src/test_mdl_storage_vfs.c
)

# test_ra8_wifi_c6link.c drives the ESP32-C6 ra8_wifi backend, which -- like the
# c6link tests above -- links libs/ra8_c6link + the vendored codec against the
# co-processor model, none of which ra8_core_hal carries. It is registered by
# hand in tests_wifi.cmake. (The pure facade test, test_ra8_wifi.c, stays in the
# auto-glob: it needs only ra8_core_hal and its own mock backend.)
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/wireless/src/test_ra8_wifi_c6link.c)

# test_app_wifi_hal_join.c drives the wifi_hal_join example's core on the host.
# It compiles the example's src/wifi_hal_core.c and adds the example include
# dir, so it is registered by hand in tests_wifi.cmake rather than the auto-glob.
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/mocks/src/test_app_wifi_hal_join.c)

foreach(src ${RA8_TEST_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  if(NOT TARGET ${name})
    ra8_add_test(${name} ${src})
  endif()
endforeach()

# Application libraries keep their host tests beside the production sources.
# Discover them before the target-specific wiring below so every if(TARGET ...)
# block configures the target it describes.  Derive the scope from the product
# tiers rather than listing library names: a new or moved library must enter
# CTest without a second registration edit.
file(
  GLOB
  APP_TEST_SOURCES
  CONFIGURE_DEPENDS
  ${FW_ROOT}/apps/board/stand_alone/*/tests/src/test_*.c
  ${FW_ROOT}/apps/shared_libs/*/tests/src/test_*.c
)

# The downloader's canonical listfile composes its focused test executables
# from deliberately different production-source closures. Treating every
# test_*.c there as a standalone ra8_core_hal test loses those closures and
# also splits helper translation units out of their owning executable. Keep
# only the five firmware/POSIX parity tests that are defined by this root suite;
# the downloader's own targets are added from its listfile below.
set(MDL_ROOT_PARITY_TEST_SOURCES
    "${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_mdl_app_storage.c"
    "${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_mdl_export_parity.c"
    "${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_mdl_library.c"
    "${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_mdl_readers.c"
    "${FW_ROOT}/apps/shared_libs/mdl/tests/src/test_mdl_state_parity.c"
)
list(
  FILTER
  APP_TEST_SOURCES
  EXCLUDE
  REGEX
  "/apps/shared_libs/mdl/tests/src/test_.*\\.c$"
)
list(APPEND APP_TEST_SOURCES ${MDL_ROOT_PARITY_TEST_SOURCES})
list(REMOVE_ITEM APP_TEST_SOURCES
     "${FW_ROOT}/apps/shared_libs/mdl_storage_vfs/tests/src/test_mdl_storage_vfs.c"
)
list(REMOVE_ITEM APP_TEST_SOURCES
     "${FW_ROOT}/apps/shared_libs/rabook_compile/tests/src/test_ra8_c6link_rabook.c"
)
foreach(src ${APP_TEST_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  if(NOT TARGET ${name})
    ra8_add_test(${name} ${src})
  endif()
endforeach()

# RA8P1 deliberately exports the same substitutable BSP names as EK-RA8D2.
# Compile its source into one focused test under private symbol spellings so the
# default EK-RA8D2 host object library can remain linked without duplicate
# definitions while the RA8P1 console path still contributes real coverage.
target_sources(
  test_ra8_board_ra8p1_cov PRIVATE ${FW_ROOT}/libs/ra8_board_ra8p1/src/ra8_board_ra8p1.c
)
target_include_directories(test_ra8_board_ra8p1_cov PRIVATE ${FW_ROOT}/libs/ra8_board_ra8p1/inc)
target_compile_definitions(
  test_ra8_board_ra8p1_cov
  PRIVATE k_ra8_board_name=k_ra8p1_test_board_name
          k_ra8_board_doc_rev=k_ra8p1_test_board_doc_rev
          k_ra8_board_mcu=k_ra8p1_test_board_mcu
          ra8_board_get_info=ra8p1_test_board_get_info
          ra8_board_led_pin=ra8p1_test_board_led_pin
          ra8_board_led_init=ra8p1_test_board_led_init
          ra8_board_led_on=ra8p1_test_board_led_on
          ra8_board_led_off=ra8p1_test_board_led_off
          ra8_board_led_toggle=ra8p1_test_board_led_toggle
          ra8_board_sw_pin=ra8p1_test_board_sw_pin
          ra8_board_sw_init=ra8p1_test_board_sw_init
          ra8_board_sw_read=ra8p1_test_board_sw_read
          ra8_board_sw_attach_irq=ra8p1_test_board_sw_attach_irq
          ra8_board_uart_console_init=ra8p1_test_board_uart_console_init
          ra8_board_uart_console_write=ra8p1_test_board_uart_console_write
          ra8_board_uart_console_read=ra8p1_test_board_uart_console_read
          ra8_board_uart_console_flush=ra8p1_test_board_uart_console_flush
)
target_link_options(
  test_ra8_board_ra8p1_cov
  PRIVATE
  -Wl,--wrap=ra8_cgc_get_clock_hz
  -Wl,--wrap=ra8_gpio_output_init
  -Wl,--wrap=ra8_gpio_input_init
  -Wl,--wrap=ra8_gpio_write
  -Wl,--wrap=ra8_gpio_toggle
  -Wl,--wrap=ra8_gpio_read
  -Wl,--wrap=ra8_icu_configure_irq_pin
  -Wl,--wrap=ra8_isr_register
  -Wl,--wrap=ra8_pfs_route_peripheral
  -Wl,--wrap=ra8_sci_init
  -Wl,--wrap=ra8_sci_write_polling
  -Wl,--wrap=ra8_sci_getc_polling
  -Wl,--wrap=ra8_sci_flush
)

# The reusable EK-RA8D2 network-provision parser lives beside the examples
# rather than under apps/shared_libs, so it is outside APP_TEST_SOURCES. Build
# the production translation unit into its focused host test explicitly; the
# standard ra8_add_test profile supplies ra8_core_hal and the host warnings.
ra8_add_test(
  test_ra8_net_provision
  ${FW_ROOT}/examples/ek_ra8d2/common/network_provision/tests/src/test_ra8_net_provision.c
)
target_sources(
  test_ra8_net_provision
  PRIVATE ${FW_ROOT}/examples/ek_ra8d2/common/network_provision/src/ra8_net_provision.c
)
target_include_directories(
  test_ra8_net_provision PRIVATE ${FW_ROOT}/examples/ek_ra8d2/common/network_provision/inc
)

# Reuse the hosted Alphabet Soup project's complete production-source closure
# and focused test target. Because this subdirectory inherits the root host
# coverage options, both the solver and its CLI wrapper contribute execution
# data to the one tree-wide trace instead of remaining a separate local build.
if(NOT TARGET test_alphabet_soup)
  add_subdirectory(${FW_ROOT}/apps/host/alphabet_soup ${CMAKE_BINARY_DIR}/_alphabet_soup)
endif()

# Reuse the downloader core's source-of-truth target composition so the root
# suite runs those tests without duplicating their private includes, fixtures,
# generated-code dependencies, or production-source closures.
set(MDL_REPO_ROOT "${FW_ROOT}")
if(NOT TARGET mdl_core)
  add_subdirectory(${FW_ROOT}/apps/shared_libs/mdl ${CMAKE_BINARY_DIR}/_mdl_core)
endif()

# This test deliberately forges logical source names with #line directives to
# pin Unity's exact diagnostics. Keep executing it in coverage builds, but do
# not ask gcov to resolve those synthetic names as repository source paths --
# gcovr cannot, and aborts the entire report rather than skipping the file.
#
# The opt-out is a property the coverage flags in host_config.cmake honour, not
# a per-target `-fno-profile-arcs -fno-test-coverage`: that pair does not undo
# `--coverage` under gcc and is a -Werror unused-argument error under clang.
# See the RA8_SKIP_COVERAGE_INSTRUMENTATION block in host_config.cmake.
if(TARGET test_ra8_unity_output)
  set_target_properties(test_ra8_unity_output PROPERTIES RA8_SKIP_COVERAGE_INSTRUMENTATION ON)
endif()

# The strict book-stream MC/DC vectors call documented private validator seams
# whose types and declarations remain outside the public library ABI. The
# strict-reader suites and the per-operand MC/DC suites are two translation
# units so neither reaches the file-size cap, and each links its own copy of
# the shared RABOOK1 fixture so neither test process depends on state owned by
# its sibling.
foreach(book_stream_test IN ITEMS test_book_stream test_book_stream_mcdc)
  if(TARGET ${book_stream_test})
    target_include_directories(${book_stream_test} PRIVATE ${FW_ROOT}/apps/shared_libs/book/src)
    target_sources(
      ${book_stream_test} PRIVATE ${FW_ROOT}/apps/shared_libs/book/tests/src/book_stream_fixture.c
    )
  endif()
endforeach()

# The DOCTYPE lexer's entry guard re-checks a nine-byte prefix that
# xml_reader_next() has already matched, so its length and mismatch
# operands are only reachable through priv_xml_doctype(), declared in the
# module's xml_internal.h (see CLAUDE.md "Test access to internal
# symbols"). Expose that src/ directory to the XML reader test alone.
if(TARGET test_xml)
  target_include_directories(test_xml PRIVATE ${FW_ROOT}/apps/shared_libs/xml/src)
endif()

# The resident and streaming RABOOK1 tests share one caller-owned fixture
# implementation. Compile it into each standalone executable so neither test
# depends on state or symbols owned by its sibling process.
foreach(rabook_compile_test IN ITEMS test_rabook_compile test_rabook_compile_stream)
  if(TARGET ${rabook_compile_test})
    target_sources(
      ${rabook_compile_test}
      PRIVATE ${FW_ROOT}/apps/shared_libs/rabook_compile/tests/src/rabook_compile_test_fixture.c
    )
  endif()
endforeach()

# The portable-filesystem conformance test runs the same vectors against the
# firmware VFS adapter (from ra8_core_hal) and this hosted POSIX port. Keep the
# POSIX source out of the firmware object library by adding it only here.
if(TARGET test_fw_if_fs)
  target_sources(
    test_fw_if_fs
    PRIVATE ${FW_ROOT}/tests/misc/src/fw_if_fs_posix_cases_test.c
            ${FW_ROOT}/tests/misc/src/fw_if_fs_posix_root_test.c
            ${FW_ROOT}/tests/support/src/fw_if_fs_contract_test.c
            ${FW_ROOT}/tests/support/src/fw_if_fs_cursor_test.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_bind.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_common.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_stream.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_hash.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_pathfs.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_sanitize.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_storage.c
  )
  target_include_directories(
    test_fw_if_fs
    PRIVATE ${FW_ROOT}/libs/if/inc
            ${FW_ROOT}/libs/if_ra8_vfs/inc
            ${FW_ROOT}/port/posix/inc
            ${FW_ROOT}/port/posix/src
            ${FW_ROOT}/apps/shared_libs/mdl/inc
  )
endif()

# The raw POSIX directory test compiles the hosted adapter with a private read
# injection seam. Production builds do not define RA8_POSIX_TEST and therefore
# contain neither the mutable seam nor its setter.
if(TARGET test_fw_if_fs_posix_raw)
  target_sources(
    test_fw_if_fs_posix_raw
    PRIVATE ${FW_ROOT}/port/posix/src/fw_if_fs_posix.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_bind.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_common.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_stream.c
  )
  target_include_directories(
    test_fw_if_fs_posix_raw PRIVATE ${FW_ROOT}/libs/if/inc ${FW_ROOT}/port/posix/inc
                                    ${FW_ROOT}/port/posix/src
  )
  target_compile_definitions(test_fw_if_fs_posix_raw PRIVATE RA8_POSIX_TEST)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_fw_if_fs_posix_raw PRIVATE RA8_POSIX_CLOSE_WRAP_TEST)
    target_link_options(test_fw_if_fs_posix_raw PRIVATE "-Wl,--wrap=close")
  endif()
endif()

# Downloader state persistence runs one journal/recovery/fault vector against
# both the hosted POSIX adapter and the firmware RAM blockdev -> FAT -> VFS
# stack. Production state sources are compiled directly into this focused
# executable so the portable contract is identical to the media tool build.
if(TARGET test_mdl_state_parity)
  target_sources(
    test_mdl_state_parity
    PRIVATE ${FW_ROOT}/port/posix/src/fw_if_fs_posix.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_bind.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_common.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_stream.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_state.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_state_codec.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_state_decimal.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_state_store.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_storage.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_hash.c
            ${FW_ROOT}/apps/shared_libs/mdl/tests/src/mdl_state_fs_fault.c
  )
  target_include_directories(
    test_mdl_state_parity
    PRIVATE ${FW_ROOT}/libs/if/inc
            ${FW_ROOT}/libs/if_ra8_vfs/inc
            ${FW_ROOT}/port/posix/inc
            ${FW_ROOT}/port/posix/src
            ${FW_ROOT}/apps/shared_libs/mdl/inc
            ${FW_ROOT}/apps/shared_libs/mdl/src
            ${CMAKE_CURRENT_SOURCE_DIR}/support/inc
  )
  target_compile_definitions(test_mdl_state_parity PRIVATE _GNU_SOURCE)
endif()

# Portable downloader library enumeration/removal runs identical authenticated,
# bounded, and fault-injected vectors over POSIX and RAM blockdev/FAT/VFS.
if(TARGET test_mdl_library)
  target_sources(
    test_mdl_library
    PRIVATE ${FW_ROOT}/port/posix/src/fw_if_fs_posix.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_bind.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_common.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_stream.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_library.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_sanitize.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_state.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_state_codec.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_state_decimal.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_state_store.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_storage.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_hash.c
            ${FW_ROOT}/apps/shared_libs/mdl/tests/src/mdl_state_fs_fault.c
  )
  target_include_directories(
    test_mdl_library
    PRIVATE ${FW_ROOT}/libs/if/inc
            ${FW_ROOT}/libs/if_ra8_vfs/inc
            ${FW_ROOT}/port/posix/inc
            ${FW_ROOT}/port/posix/src
            ${FW_ROOT}/apps/shared_libs/mdl/inc
            ${FW_ROOT}/apps/shared_libs/mdl/src
            ${CMAKE_CURRENT_SOURCE_DIR}/support/inc
  )
  target_compile_definitions(test_mdl_library PRIVATE _GNU_SOURCE)
endif()

# Downloader config and image readers execute identical bounded/fault vectors
# over the hosted POSIX port and the firmware RAM blockdev -> FAT -> VFS stack.
if(TARGET test_mdl_readers)
  target_sources(
    test_mdl_readers
    PRIVATE ${FW_ROOT}/port/posix/src/fw_if_fs_posix.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_bind.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_common.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_stream.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_config.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_urlname.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_export_workspace.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_verify.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_verify_tarball.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_verify_rabook.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_sanitize.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_storage.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_hash.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_politeness.c
            ${FW_ROOT}/apps/shared_libs/mdl/tests/src/mdl_state_fs_fault.c
  )
  target_include_directories(
    test_mdl_readers
    PRIVATE ${FW_ROOT}/libs/if/inc
            ${FW_ROOT}/libs/if_ra8_vfs/inc
            ${FW_ROOT}/apps/shared_libs/mdl/inc
            ${FW_ROOT}/port/posix/inc
            ${FW_ROOT}/port/posix/src
            ${FW_ROOT}/apps/shared_libs/mdl/inc
            ${FW_ROOT}/apps/shared_libs/mdl/src
            ${CMAKE_CURRENT_SOURCE_DIR}/support/inc
  )
  target_compile_definitions(test_mdl_readers PRIVATE _GNU_SOURCE)
endif()

# Portable media application storage uses the same directory, regular-file
# removal, and validated create-new transaction policies over POSIX and the
# real RAM blockdev -> FAT -> VFS stack. The fault wrapper injects every
# publication phase while production code preserves pre-existing destinations.
if(TARGET test_mdl_app_storage)
  target_sources(
    test_mdl_app_storage
    PRIVATE ${FW_ROOT}/port/posix/src/fw_if_fs_posix.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_bind.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_common.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_stream.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_app_storage.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_storage.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_hash.c
            ${FW_ROOT}/apps/shared_libs/mdl/tests/src/mdl_state_fs_fault.c
  )
  # mdl_app_storage is the application layer's own portable storage policy, and
  # it followed its subject down into apps/shared_libs/ with the rest of that layer
  # (#725), so this target now names no build form at all.
  target_include_directories(
    test_mdl_app_storage
    PRIVATE ${FW_ROOT}/libs/if/inc
            ${FW_ROOT}/libs/if_ra8_vfs/inc
            ${FW_ROOT}/port/posix/inc
            ${FW_ROOT}/port/posix/src
            ${FW_ROOT}/apps/shared_libs/mdl/inc
            ${FW_ROOT}/apps/shared_libs/mdl/src
            ${CMAKE_CURRENT_SOURCE_DIR}/support/inc
  )
  target_compile_definitions(test_mdl_app_storage PRIVATE _GNU_SOURCE)
endif()

# Every media exporter runs unchanged over both the hosted POSIX adapter and
# the real RAM blockdev -> FAT -> VFS firmware stack. Format readers validate
# each borrowed transaction stage before publication and again after commit.
if(TARGET test_mdl_export_parity)
  target_sources(
    test_mdl_export_parity
    PRIVATE ${FW_ROOT}/port/posix/src/fw_if_fs_posix.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_bind.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_common.c
            ${FW_ROOT}/port/posix/src/fw_if_fs_posix_stream.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_export.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_export_workspace.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_export_io.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_export_meta.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_export_zip.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_export_tar.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_export_epub.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_export_epub_meta.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_export_jof.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_export_rabook.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_export_rabook_io.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_verify.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_verify_tarball.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_verify_rabook.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_urlname.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_url_guard.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_sanitize.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_storage.c
            ${FW_ROOT}/apps/shared_libs/mdl/src/mdl_hash.c
  )
  target_include_directories(
    test_mdl_export_parity
    PRIVATE ${FW_ROOT}/libs/if/inc
            ${FW_ROOT}/libs/if_ra8_vfs/inc
            ${FW_ROOT}/apps/shared_libs/mdl/inc
            ${FW_ROOT}/port/posix/inc
            ${FW_ROOT}/port/posix/src
            ${FW_ROOT}/apps/shared_libs/mdl/inc
            ${FW_ROOT}/apps/shared_libs/mdl/src
            ${FW_ROOT}/apps/shared_libs/mdl/tests/inc
  )
  target_compile_definitions(test_mdl_export_parity PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
endif()

# ---------------------------------------------------------------------------
# test_app_ereader_manga: the ereader_manga host twin drives the app's shared
# pan/zoom presentation model directly, so compile the app's src/mg_reader.c into
# the test target (and expose its inc/) -- the tested render IS the production
# render. The JOF / gfx / tile-cache libraries it calls already come from
# the ra8_core_hal object library the auto-glob linked.
# ---------------------------------------------------------------------------
if(TARGET test_app_ereader_manga)
  target_sources(
    test_app_ereader_manga
    PRIVATE ${FW_ROOT}/examples/ek_ra8d2/hw_pending/ereader_manga/src/mg_reader.c
  )
  target_include_directories(
    test_app_ereader_manga PRIVATE ${FW_ROOT}/examples/ek_ra8d2/hw_pending/ereader_manga/inc
  )
endif()

if(TARGET test_ereader_pageturn)
  target_include_directories(
    test_ereader_pageturn PRIVATE ${FW_ROOT}/examples/ek_ra8d2/hw_validated/hil/ereader_ui/inc
  )
endif()

foreach(shelf_test IN ITEMS test_ra8_shelf_classify test_app_ereader_shelf_comic)
  if(TARGET ${shelf_test})
    target_include_directories(
      ${shelf_test} PRIVATE ${FW_ROOT}/examples/ek_ra8d2/hil_needs_revalidation/ereader_shelf/inc
    )
  endif()
endforeach()

# ---------------------------------------------------------------------------
# test_app_ereader_zoom: the ereader_zoom host twin drives the app's own
# ez_scene.c, so the four framebuffer hashes it asserts come out of the
# PRODUCTION render rather than a re-implementation -- which is the only thing
# that makes them a golden for hil.conf's banner. zoom / ra8_gfx /
# ra8_mem already come from the ra8_core_hal object library the auto-glob linked.
# ---------------------------------------------------------------------------
if(TARGET test_app_ereader_zoom)
  target_sources(
    test_app_ereader_zoom
    PRIVATE ${FW_ROOT}/examples/ek_ra8d2/hw_pending/ereader_zoom/src/ez_scene.c
  )
  target_include_directories(
    test_app_ereader_zoom PRIVATE ${FW_ROOT}/examples/ek_ra8d2/hw_pending/ereader_zoom/inc
  )
endif()

# ---------------------------------------------------------------------------
# test_app_blink_m33_hal: the blink_m33_hal host twin drives the example's own
# blink_m33_hal.h step (the exact function the CPU1 firmware loop calls), so the
# LED1 PCNTR1 effect it asserts comes out of the PRODUCTION step rather than a
# re-implementation. ra8_pcntr.h / the PORT layer already come from the
# ra8_core_hal object library the auto-glob linked; only the app's include dir
# is added so blink_m33_hal.h resolves (issue #580).
# ---------------------------------------------------------------------------
if(TARGET test_app_blink_m33_hal)
  target_include_directories(
    test_app_blink_m33_hal PRIVATE ${FW_ROOT}/examples/ek_ra8d2/hw_pending/blink_m33_hal/inc
  )
endif()

# The hosted stream adapter is compiled only into its focused target: firmware
# compositions never acquire POSIX descriptor dependencies transitively.
if(TARGET test_ra8_io_stream_posix)
  target_sources(test_ra8_io_stream_posix PRIVATE ${FW_ROOT}/port/posix/src/ra8_io_stream_posix.c)
  target_include_directories(
    test_ra8_io_stream_posix PRIVATE ${FW_ROOT}/port/posix/inc ${FW_ROOT}/port/posix/src
  )
endif()

# The strict book-stream MC/DC vectors call documented private validator seams
# whose types and declarations remain outside the public library ABI. The
# strict-reader suites and the per-operand MC/DC suites are two translation
# units so neither reaches the file-size cap, and each links its own copy of
# the shared RABOOK1 fixture so neither test process depends on state owned by
# its sibling.
foreach(book_stream_test IN ITEMS test_book_stream test_book_stream_mcdc)
  if(TARGET ${book_stream_test})
    target_include_directories(${book_stream_test} PRIVATE ${FW_ROOT}/apps/shared_libs/book/src)
    target_sources(
      ${book_stream_test} PRIVATE ${FW_ROOT}/apps/shared_libs/book/tests/src/book_stream_fixture.c
    )
  endif()
endforeach()

# The resident and streaming RABOOK1 tests share one caller-owned fixture
# implementation. Compile it into each standalone executable so neither test
# depends on state or symbols owned by its sibling process.
foreach(rabook_compile_test IN ITEMS test_rabook_compile test_rabook_compile_stream)
  if(TARGET ${rabook_compile_test})
    target_sources(
      ${rabook_compile_test}
      PRIVATE ${FW_ROOT}/apps/shared_libs/rabook_compile/tests/src/rabook_compile_test_fixture.c
    )
  endif()
endforeach()

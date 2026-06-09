cmake_minimum_required(VERSION 3.20)

#
# ra8d2-firmware host-compiled unit test build.
#
# Compiles every library source file with the host (x86_64) compiler
# so clang-tidy has a valid compile_commands.json and so actual unit
# tests can link against mock peripheral implementations.
#
# No target cross-compilation happens here -- that lives in the
# top-level CMakeLists.txt.
#
# Usage from the firmware root:
#
#     cmake -B build/tidy -S tests -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
#     cmake --build build/tidy
#
# Or via:
#
#     bash scripts/clang_tidy.sh --check
#

project(ra8d2-firmware-tests
    LANGUAGES C
    VERSION 0.1.0
    DESCRIPTION "Host-compiled unit tests for ra8d2-firmware"
)

# Host C23 build -- the target uses the same standard.
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ra_warnings.cmake)

# Every TU is compiled with RA_SIMULATOR_MODE so code that normally
# reaches into MCU register blocks takes the host-safe path instead.
add_compile_definitions(RA_SIMULATOR_MODE UNIT_TEST)

# Host warnings are applied per target below. We deliberately do NOT use
# -Wstack-usage=2200 here because the host compiler ABI pushes wider
# arguments than Cortex-M85.

# Coverage instrumentation -- gcovr / lcov friendly.
option(RA_COVERAGE "Emit gcov/lcov coverage data" ON)
option(RA_MCDC     "Emit clang source-based coverage with MC/DC (DO-178C Level B)" OFF)

# RA_MCDC takes precedence over RA_COVERAGE: the two instrumentation
# schemes are mutually exclusive (clang's source-based coverage and
# gcov-style arc/line counters use different runtime libraries).
#
# DO-178C Level B requires Modified Condition/Decision Coverage. The
# only open-source path is clang >= 18 with -fcoverage-mcdc paired
# with -fcoverage-mapping + -fprofile-instr-generate, then reported
# via `llvm-cov show --show-mcdc-summary`.
#
# We probe the host compiler at configure time. If MC/DC is available
# we wire it in for first-party sources only (third_party/ is opted
# out -- see scripts/utils/mcdc_report.sh for the SOUP exemption
# policy). If MC/DC is unavailable we fall back to gcc 14's
# `-fcondition-coverage` (decision/condition coverage, NOT MC/DC) and
# print a loud warning so callers know they are NOT measuring MC/DC.
if(RA_MCDC)
  set(RA_COVERAGE OFF CACHE BOOL "Disabled because RA_MCDC=ON" FORCE)

  # Probe: does the active C compiler accept the MC/DC flag trio?
  include(CheckCCompilerFlag)
  set(CMAKE_REQUIRED_FLAGS "-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc")
  check_c_compiler_flag("-fcoverage-mcdc" RA_HAVE_CLANG_MCDC)
  unset(CMAKE_REQUIRED_FLAGS)

  if(RA_HAVE_CLANG_MCDC)
    message(STATUS "RA_MCDC: clang MC/DC instrumentation ENABLED "
                   "(-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc)")
    set(RA_MCDC_C_FLAGS
        -O0 -g
        -fprofile-instr-generate
        -fcoverage-mapping
        -fcoverage-mcdc
    )
    add_link_options(-fprofile-instr-generate -fcoverage-mapping)
  else()
    # gcc 14+ ships -fcondition-coverage. Probe for it as a best-effort
    # fallback. This is NOT MC/DC -- it is condition coverage only.
    set(CMAKE_REQUIRED_FLAGS "-fprofile-arcs -ftest-coverage -fcondition-coverage")
    check_c_compiler_flag("-fcondition-coverage" RA_HAVE_GCC_CONDCOV)
    unset(CMAKE_REQUIRED_FLAGS)

    message(WARNING
      "RA_MCDC requested but the host compiler does not support "
      "clang's -fcoverage-mcdc. DO-178C Level B MC/DC CANNOT be "
      "measured with this toolchain. "
      "Install clang >= 18 (brew install llvm / apt install clang-18) "
      "and re-run.")

    if(RA_HAVE_GCC_CONDCOV)
      message(WARNING
        "Falling back to gcc -fcondition-coverage (condition coverage "
        "only, NOT MC/DC).")
      set(RA_MCDC_C_FLAGS
          -O0 -g --coverage
          -fprofile-arcs -ftest-coverage -fcondition-coverage
      )
      add_link_options(--coverage -fprofile-arcs -ftest-coverage)
    else()
      message(WARNING
        "Falling back to plain gcov decision/branch coverage. No "
        "condition-level data will be produced.")
      set(RA_MCDC_C_FLAGS
          -O0 -g --coverage -fprofile-arcs -ftest-coverage
      )
      add_link_options(--coverage -fprofile-arcs -ftest-coverage)
    endif()
  endif()

  # Apply globally so test executables get the same flags as the
  # ra_core_hal OBJECT library. Per-source opt-out for third_party
  # is performed below, after the source lists are populated.
  add_compile_options(${RA_MCDC_C_FLAGS})
elseif(RA_COVERAGE)
  add_compile_options(-O0 -g --coverage -fprofile-arcs -ftest-coverage)
  add_link_options(--coverage -fprofile-arcs -ftest-coverage)
endif()

# Firmware root (one directory up from tests/).
get_filename_component(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Collect every library source. Each .c file becomes part of the
# `ra_core_hal` static library. clang-tidy then walks the associated
# compile_commands.json.
file(GLOB_RECURSE RA_CORE_SOURCES    CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_core/src/*.c)
file(GLOB_RECURSE RA_HAL_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_hal/src/*.c)
file(GLOB_RECURSE RA_NET_PAL_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_net_pal/src/*.c)
file(GLOB_RECURSE RA_MODEM_AT_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_modem_at/src/*.c)
file(GLOB_RECURSE RA_DA16600_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_da16600/src/*.c)
file(GLOB_RECURSE RA_BLE_HOST_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_ble_host/src/*.c)
file(GLOB_RECURSE RA_TLS_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_tls/src/*.c)
file(GLOB_RECURSE RA_USB_PAL_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_usb_pal/src/*.c)
file(GLOB_RECURSE RA_FS_SOURCES      CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_fs/src/*.c)
file(GLOB_RECURSE RA_SDMMC_SPI_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_sdmmc_spi/src/*.c)
file(GLOB_RECURSE RA_GFX_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_gfx/src/*.c)
file(GLOB_RECURSE RA_UI_SOURCES      CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_ui/src/*.c)
file(GLOB_RECURSE RA_BOX_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_box/src/*.c)
file(GLOB_RECURSE RA_NSC_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_nsc/src/*.c)
file(GLOB_RECURSE RA_OTA_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_ota/src/*.c)
file(GLOB_RECURSE RA_DISPLAY_PAL_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_display_pal/src/*.c)
file(GLOB_RECURSE RA_POWER_PROFILE_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_power_profile/src/*.c)
file(GLOB_RECURSE RA_EPUB_C_SOURCES  CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_epub/src/*.c)
file(GLOB_RECURSE RA_EPUB_CPP_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_epub/src/*.cpp)
# ra_reflow has two implementations: v1 (hand-rolled, default) and v2
# (LiteHTML-backed, RA_REFLOW_USE_LITEHTML=ON). Only one set is compiled.
option(RA_REFLOW_USE_LITEHTML "Use the LiteHTML v2 ra_reflow engine" OFF)
if(RA_REFLOW_USE_LITEHTML)
    set(RA_REFLOW_C_SOURCES    "")
    set(RA_REFLOW_CPP_SOURCES  ${FW_ROOT}/libs/ra_reflow/v2/ra_reflow_v2.cpp)
    enable_language(CXX)
    set(_RA_SAVED_C_FLAGS   "${CMAKE_C_FLAGS}")
    set(_RA_SAVED_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "-Werror" "" CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}")
    string(REPLACE "-Werror" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    if(NOT TARGET litehtml)
      add_subdirectory(
          ${FW_ROOT}/libs/third_party/litehtml
          ${CMAKE_BINARY_DIR}/_litehtml
          EXCLUDE_FROM_ALL
      )
    endif()
    if(TARGET litehtml)
        target_compile_options(litehtml PRIVATE -w)
    endif()
    if(TARGET gumbo)
        target_compile_options(gumbo PRIVATE -w)
    endif()
    set(CMAKE_C_FLAGS   "${_RA_SAVED_C_FLAGS}")
    set(CMAKE_CXX_FLAGS "${_RA_SAVED_CXX_FLAGS}")
else()
    file(GLOB_RECURSE RA_REFLOW_C_SOURCES   CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_reflow/src/*.c)
    file(GLOB_RECURSE RA_REFLOW_CPP_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_reflow/src/*.cpp)
endif()
set(RA_EPUB_THIRD_PARTY
    ${FW_ROOT}/libs/third_party/miniz/miniz.c
    ${FW_ROOT}/libs/third_party/tinyxml2/tinyxml2.cpp
    ${FW_ROOT}/libs/third_party/stb/stb_truetype_impl.c
)
file(GLOB_RECURSE RA_SECURE_APP_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/src/secure_app/*.c)
file(GLOB_RECURSE RA_TOUCH_CAL_SOURCES  CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_touch_cal/src/*.c)
file(GLOB_RECURSE RA_PSA_CRYPTO_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_psa_crypto/src/*.c)
file(GLOB_RECURSE RA_WDT_SUPERVISOR_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_wdt_supervisor/src/*.c)
file(GLOB_RECURSE RA_MPU_SOURCES        CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_mpu/src/*.c)
file(GLOB_RECURSE RA_BOARD_EK_RA8D2_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_board_ek_ra8d2/src/*.c)
file(GLOB_RECURSE RA_LSM6DSO_SOURCES        CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_lsm6dso/src/*.c)
file(GLOB_RECURSE RA_TZ_SECURE_BOOT_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_tz_secure_boot/src/*.c)

add_library(ra_core_hal OBJECT
    ${RA_CORE_SOURCES}
    ${RA_HAL_SOURCES}
    ${RA_NET_PAL_SOURCES}
    ${RA_MODEM_AT_SOURCES}
    ${RA_DA16600_SOURCES}
    ${RA_BLE_HOST_SOURCES}
    ${RA_TLS_SOURCES}
    ${RA_USB_PAL_SOURCES}
    ${RA_FS_SOURCES}
    ${RA_SDMMC_SPI_SOURCES}
    ${RA_GFX_SOURCES}
    ${RA_UI_SOURCES}
    ${RA_BOX_SOURCES}
    ${RA_NSC_SOURCES}
    ${RA_OTA_SOURCES}
    ${RA_DISPLAY_PAL_SOURCES}
    ${RA_POWER_PROFILE_SOURCES}
    ${RA_EPUB_C_SOURCES}
    ${RA_EPUB_CPP_SOURCES}
    ${RA_REFLOW_C_SOURCES}
    ${RA_REFLOW_CPP_SOURCES}
    ${RA_EPUB_THIRD_PARTY}
    ${RA_SECURE_APP_SOURCES}
    ${RA_TOUCH_CAL_SOURCES}
    ${RA_MPU_SOURCES}
    ${RA_PSA_CRYPTO_SOURCES}
    ${RA_WDT_SUPERVISOR_SOURCES}
    ${RA_BOARD_EK_RA8D2_SOURCES}
    ${RA_LSM6DSO_SOURCES}
    ${RA_TZ_SECURE_BOOT_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_mmap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_irq.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_dma.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_world.c
)
ra_target_enable_project_warnings(ra_core_hal)
target_compile_options(ra_core_hal PRIVATE
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-address-of-packed-member
)
target_include_directories(ra_core_hal PUBLIC
    ${FW_ROOT}/libs/ra_core/inc
    ${FW_ROOT}/libs/ra_hal/inc
    ${FW_ROOT}/libs/ra_net_pal/inc
    ${FW_ROOT}/libs/ra_modem_at/inc
    ${FW_ROOT}/libs/ra_da16600/inc
    ${FW_ROOT}/libs/ra_ble_host/inc
    ${FW_ROOT}/libs/ra_usb_pal/inc
    ${FW_ROOT}/libs/ra_fs/inc
    ${FW_ROOT}/libs/ra_sdmmc_spi/inc
    ${FW_ROOT}/libs/ra_gfx/inc
    ${FW_ROOT}/libs/ra_ui/inc
    ${FW_ROOT}/libs/ra_box/inc
    ${FW_ROOT}/libs/ra_tls/inc
    ${FW_ROOT}/libs/ra_nsc/inc
    ${FW_ROOT}/libs/ra_ota/inc
    ${FW_ROOT}/libs/ra_display_pal/inc
    ${FW_ROOT}/libs/ra_power_profile/inc
    ${FW_ROOT}/libs/ra_epub/inc
    ${FW_ROOT}/libs/ra_reflow/inc
    ${FW_ROOT}/libs/ra_touch_cal/inc
    ${FW_ROOT}/libs/ra_mpu/inc
    ${FW_ROOT}/libs/ra_psa_crypto/inc
    ${FW_ROOT}/libs/ra_wdt_supervisor/inc
    ${FW_ROOT}/libs/ra_board_ek_ra8d2/inc
    ${FW_ROOT}/libs/ra_lsm6dso/inc
    ${FW_ROOT}/libs/ra_tz_secure_boot/inc
    ${FW_ROOT}/libs/third_party/miniz
    ${FW_ROOT}/libs/third_party/tinyxml2
    ${FW_ROOT}/libs/third_party/stb
    ${FW_ROOT}/src
    ${FW_ROOT}/src/inc
    ${FW_ROOT}/src/secure_app
    # Per-module src/ directories exposed for MC/DC test access to
    # internal helpers (see CLAUDE.md "Test access to internal symbols").
    # Tests under tests/ MAY include "<module>_internal.h" to drive MC/DC
    # vectors on production source text. Only first-party libs are listed.
    ${FW_ROOT}/libs/ra_core/src
    ${FW_ROOT}/libs/ra_hal/src
    ${FW_ROOT}/libs/ra_net_pal/src
    ${FW_ROOT}/libs/ra_modem_at/src
    ${FW_ROOT}/libs/ra_da16600/src
    ${FW_ROOT}/libs/ra_ble_host/src
    ${FW_ROOT}/libs/ra_tls/src
    ${FW_ROOT}/libs/ra_usb_pal/src
    ${FW_ROOT}/libs/ra_fs/src
    ${FW_ROOT}/libs/ra_sdmmc_spi/src
    ${FW_ROOT}/libs/ra_gfx/src
    ${FW_ROOT}/libs/ra_nsc/src
    ${FW_ROOT}/libs/ra_ota/src
    ${FW_ROOT}/libs/ra_display_pal/src
    ${FW_ROOT}/libs/ra_power_profile/src
    ${FW_ROOT}/libs/ra_touch_cal/src
    ${FW_ROOT}/libs/ra_mpu/src
    ${FW_ROOT}/libs/ra_psa_crypto/src
    ${FW_ROOT}/libs/ra_wdt_supervisor/src
    ${FW_ROOT}/libs/ra_board_ek_ra8d2/src
    ${FW_ROOT}/libs/ra_lsm6dso/src
    ${FW_ROOT}/libs/ra_epub/src
    ${FW_ROOT}/libs/ra_reflow/src
    ${FW_ROOT}/libs/ra_tz_secure_boot/src
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
# tinyxml2.cpp + ra_epub_xml_shim.cpp are C++; everything else is C.
# Override per-source language flags so cmake doesn't pass -std=c23 to the
# C++ compiler.
set_source_files_properties(
    ${FW_ROOT}/libs/third_party/tinyxml2/tinyxml2.cpp
    ${RA_EPUB_CPP_SOURCES}
    ${RA_REFLOW_CPP_SOURCES}
    PROPERTIES LANGUAGE CXX
)
set_source_files_properties(
    ${RA_EPUB_THIRD_PARTY}
    PROPERTIES COMPILE_OPTIONS -w
)

# Strip MC/DC / coverage instrumentation from vendored third_party
# sources. Under DO-178C these are SOUP (Software Of Unknown
# Pedigree) and are exempted from MC/DC measurement -- see
# docs/MCDC.md. Per-file COMPILE_OPTIONS overrides the directory
# add_compile_options() set above when RA_MCDC=ON.
if(RA_MCDC)
  set(RA_MCDC_THIRD_PARTY_SOURCES
      ${RA_EPUB_THIRD_PARTY}
  )
  foreach(_src ${RA_MCDC_THIRD_PARTY_SOURCES})
    set_property(SOURCE ${_src} APPEND PROPERTY COMPILE_OPTIONS
        -fno-coverage-mcdc
        -fno-coverage-mapping
        -fno-profile-instr-generate
        -w
    )
  endforeach()
endif()
if(RA_REFLOW_USE_LITEHTML)
    set_source_files_properties(
        ${FW_ROOT}/libs/ra_reflow/v2/ra_reflow_v2.cpp
        PROPERTIES COMPILE_OPTIONS -w
    )
    target_include_directories(ra_core_hal PUBLIC
        ${FW_ROOT}/libs/third_party/litehtml/include
    )
    target_link_libraries(ra_core_hal PUBLIC litehtml gumbo)
endif()
target_compile_features(ra_core_hal PUBLIC cxx_std_17)

# C++ enabled here only (the top-level project is C/ASM-only since the
# cross-compile path doesn't ship a C++ runtime). tinyxml2 + ra_epub_xml_shim
# in libs/ra_epub need C++17 for tinyxml2.
enable_language(CXX)

# Enable ctest so `cmake --build . --target test` and bash helpers work.
enable_testing()

# ---------------------------------------------------------------------------
# Unit tests -- one executable per test_*.c file. Each test links
# against the ra_core_hal OBJECT library and runs standalone.
# ---------------------------------------------------------------------------

function(ra_add_test name)
  add_executable(${name} ${name}.c $<TARGET_OBJECTS:ra_core_hal>)
  target_compile_options(${name} PRIVATE
    -Wall
    -Wextra
    -Werror
    # GUIX retirement (issue #81): host tests that exercise the deprecated
    # GUIX seam (e.g. test_ra_display_pal.c -> display_bind_guix) must still
    # build under -Werror. Remove with the GUIX stack.
    -Wno-error=deprecated-declarations
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-address-of-packed-member
  )
  set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)
  target_include_directories(${name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FW_ROOT}/libs/ra_core/inc
    ${FW_ROOT}/libs/ra_hal/inc
    ${FW_ROOT}/libs/ra_net_pal/inc
    ${FW_ROOT}/libs/ra_modem_at/inc
    ${FW_ROOT}/libs/ra_da16600/inc
    ${FW_ROOT}/libs/ra_ble_host/inc
    ${FW_ROOT}/libs/ra_usb_pal/inc
    ${FW_ROOT}/libs/ra_fs/inc
    ${FW_ROOT}/libs/ra_sdmmc_spi/inc
    ${FW_ROOT}/libs/ra_gfx/inc
    ${FW_ROOT}/libs/ra_ui/inc
    ${FW_ROOT}/libs/ra_box/inc
    ${FW_ROOT}/libs/ra_tls/inc
    ${FW_ROOT}/libs/ra_nsc/inc
    ${FW_ROOT}/libs/ra_ota/inc
    ${FW_ROOT}/libs/ra_display_pal/inc
    ${FW_ROOT}/libs/ra_power_profile/inc
    ${FW_ROOT}/libs/ra_epub/inc
    ${FW_ROOT}/libs/ra_reflow/inc
    ${FW_ROOT}/libs/ra_touch_cal/inc
    ${FW_ROOT}/libs/ra_mpu/inc
    ${FW_ROOT}/libs/ra_psa_crypto/inc
    ${FW_ROOT}/libs/ra_wdt_supervisor/inc
    ${FW_ROOT}/libs/ra_board_ek_ra8d2/inc
    ${FW_ROOT}/libs/ra_lsm6dso/inc
    ${FW_ROOT}/libs/ra_tz_secure_boot/inc
    ${FW_ROOT}/libs/third_party/miniz
    ${FW_ROOT}/libs/third_party/tinyxml2
    ${FW_ROOT}/libs/third_party/stb
    ${FW_ROOT}/src/secure_app
    # Per-module src/ for MC/DC test access (see CLAUDE.md).
    ${FW_ROOT}/libs/ra_core/src
    ${FW_ROOT}/libs/ra_hal/src
    ${FW_ROOT}/libs/ra_net_pal/src
    ${FW_ROOT}/libs/ra_modem_at/src
    ${FW_ROOT}/libs/ra_da16600/src
    ${FW_ROOT}/libs/ra_ble_host/src
    ${FW_ROOT}/libs/ra_tls/src
    ${FW_ROOT}/libs/ra_usb_pal/src
    ${FW_ROOT}/libs/ra_fs/src
    ${FW_ROOT}/libs/ra_sdmmc_spi/src
    ${FW_ROOT}/libs/ra_gfx/src
    ${FW_ROOT}/libs/ra_nsc/src
    ${FW_ROOT}/libs/ra_ota/src
    ${FW_ROOT}/libs/ra_display_pal/src
    ${FW_ROOT}/libs/ra_power_profile/src
    ${FW_ROOT}/libs/ra_touch_cal/src
    ${FW_ROOT}/libs/ra_mpu/src
    ${FW_ROOT}/libs/ra_psa_crypto/src
    ${FW_ROOT}/libs/ra_wdt_supervisor/src
    ${FW_ROOT}/libs/ra_board_ek_ra8d2/src
    ${FW_ROOT}/libs/ra_lsm6dso/src
    ${FW_ROOT}/libs/ra_epub/src
    ${FW_ROOT}/libs/ra_reflow/src
    ${FW_ROOT}/libs/ra_tz_secure_boot/src
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  )
  if(APPLE)
    target_link_options(${name} PRIVATE
      "-Wl,-pagezero_size,0x4000"
      "-Wl,-segalign,0x4000"
    )
  endif()
  if(RA_REFLOW_USE_LITEHTML)
    # ra_core_hal is an OBJECT library, so link deps don't propagate
    # automatically; bind litehtml/gumbo directly into each test exe
    # so the v2 adapter's symbols resolve.
    target_link_libraries(${name} PRIVATE litehtml gumbo)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# Auto-discover every test_*.c file in this directory and register it
# via ra_add_test(). Dropping a new test_foo.c file is enough -- no
# manual list edit required. (CONFIGURE_DEPENDS means CMake re-globs
# on the next build if the directory contents changed.)
file(GLOB RA_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)

# v1's test_ra_reflow.c asserts on glyph-array internals that v2
# deliberately does not populate; drop it under v2 and add the
# C++ v2 test below in its place.
if(RA_REFLOW_USE_LITEHTML)
  list(REMOVE_ITEM RA_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_ra_reflow.c)
endif()

# test_coverage_compile_all.c is the coverage aggregator: a single
# no-op TU whose only purpose is to force every first-party source
# to be linked with the active coverage instrumentation flags so
# llvm-cov / gcovr can compute uniform repository-wide coverage.
# It contributes zero functional test value, so we exclude it from
# the fast `make test` flow (RA_MCDC=OFF) and only build it when
# `RA_MCDC=ON`.
if(NOT RA_MCDC)
  list(REMOVE_ITEM RA_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_coverage_compile_all.c)
endif()

foreach(src ${RA_TEST_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  ra_add_test(${name})
endforeach()

if(RA_REFLOW_USE_LITEHTML)
  add_executable(test_ra_reflow_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/test_ra_reflow_v2.cpp
    $<TARGET_OBJECTS:ra_core_hal>
  )
  set_target_properties(test_ra_reflow_v2 PROPERTIES LINKER_LANGUAGE CXX)
  target_compile_options(test_ra_reflow_v2 PRIVATE
    -Wall -Wextra -Wno-unused-function -Wno-unused-parameter
    -Wno-unused-variable -Wno-address-of-packed-member
  )
  target_compile_features(test_ra_reflow_v2 PRIVATE cxx_std_17)
  target_include_directories(test_ra_reflow_v2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FW_ROOT}/libs/ra_core/inc
    ${FW_ROOT}/libs/ra_reflow/inc
    ${FW_ROOT}/libs/third_party/litehtml/include
    ${FW_ROOT}/libs/third_party/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  )
  target_link_libraries(test_ra_reflow_v2 PRIVATE litehtml gumbo)
  if(APPLE)
    target_link_options(test_ra_reflow_v2 PRIVATE
      "-Wl,-pagezero_size,0x4000"
      "-Wl,-segalign,0x4000"
    )
  endif()
  add_test(NAME test_ra_reflow_v2 COMMAND test_ra_reflow_v2)
endif()

# ---------------------------------------------------------------------------
# C++ test target for ra_epub_xml_shim. The XML shim is a C++ TU that is
# compiled into the ra_core_hal OBJECT library above; this executable
# drives its public C-linkage entry points to land MC/DC vectors on the
# decisions documented in tests/test_ra_epub_xml_shim.cpp.
# ---------------------------------------------------------------------------
add_executable(test_ra_epub_xml_shim
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra_epub_xml_shim.cpp
  $<TARGET_OBJECTS:ra_core_hal>
)
set_target_properties(test_ra_epub_xml_shim PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_ra_epub_xml_shim PRIVATE
  -Wall -Wextra -Wno-unused-function -Wno-unused-parameter
  -Wno-unused-variable -Wno-address-of-packed-member
)
target_compile_features(test_ra_epub_xml_shim PRIVATE cxx_std_17)
target_include_directories(test_ra_epub_xml_shim PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${FW_ROOT}/libs/ra_core/inc
  ${FW_ROOT}/libs/ra_epub/inc
  ${FW_ROOT}/libs/third_party/tinyxml2
  ${FW_ROOT}/libs/third_party/stb
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
if(RA_REFLOW_USE_LITEHTML)
  target_link_libraries(test_ra_epub_xml_shim PRIVATE litehtml gumbo)
endif()
if(APPLE)
  target_link_options(test_ra_epub_xml_shim PRIVATE
    "-Wl,-pagezero_size,0x4000"
    "-Wl,-segalign,0x4000"
  )
endif()
add_test(NAME test_ra_epub_xml_shim COMMAND test_ra_epub_xml_shim)

# ---------------------------------------------------------------------------
# C++ test target for ra_reflow_xml_shim. Mirrors the ra_epub variant: drive
# the TU-private helpers exposed via ra_reflow_xml_shim_internal.hpp directly
# so MC/DC vectors land on the production decisions at lines 70/72/232/334.
# ---------------------------------------------------------------------------
add_executable(test_ra_reflow_xml_shim
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra_reflow_xml_shim.cpp
  $<TARGET_OBJECTS:ra_core_hal>
)
set_target_properties(test_ra_reflow_xml_shim PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_ra_reflow_xml_shim PRIVATE
  -Wall -Wextra -Wno-unused-function -Wno-unused-parameter
  -Wno-unused-variable -Wno-address-of-packed-member
)
target_compile_features(test_ra_reflow_xml_shim PRIVATE cxx_std_17)
target_include_directories(test_ra_reflow_xml_shim PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${FW_ROOT}/libs/ra_core/inc
  ${FW_ROOT}/libs/ra_reflow/inc
  ${FW_ROOT}/libs/ra_reflow/src
  ${FW_ROOT}/libs/third_party/tinyxml2
  ${FW_ROOT}/libs/third_party/stb
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
if(RA_REFLOW_USE_LITEHTML)
  target_link_libraries(test_ra_reflow_xml_shim PRIVATE litehtml gumbo)
endif()
if(APPLE)
  target_link_options(test_ra_reflow_xml_shim PRIVATE
    "-Wl,-pagezero_size,0x4000"
    "-Wl,-segalign,0x4000"
  )
endif()
add_test(NAME test_ra_reflow_xml_shim COMMAND test_ra_reflow_xml_shim)

# ---------------------------------------------------------------------------
# Optional libFuzzer harnesses (opt-in via -DRA_FUZZ=ON; clang-only).
# Each fuzz target is built as a separate executable that links against
# the existing ra_core_hal OBJECT library and adds
# -fsanitize=fuzzer,address,undefined only on its own translation unit.
# Default OFF so the normal host test build is unaffected.
# See docs/FUZZING.md.
# ---------------------------------------------------------------------------
option(RA_FUZZ "Build libFuzzer harnesses for parsers (clang only)" OFF)
add_subdirectory(fuzz)

# ---------------------------------------------------------------------------
# Optional host-side performance benchmarks (opt-in via -DRA_BENCH=ON).
# Each bench target prints CSV results to stdout. Decoupled from the
# fast unit-test build because the bench binaries build at -O2 and we
# do not want them entangled with coverage/MC-DC instrumentation.
# See docs/PERFORMANCE.md.
# ---------------------------------------------------------------------------
option(RA_BENCH "Build host-side performance microbenchmarks" OFF)
add_subdirectory(bench)

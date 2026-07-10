cmake_minimum_required(VERSION 3.20)

#
# ra8-firmware host-compiled unit test build.
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

project(ra8-firmware-tests
    LANGUAGES C
    VERSION 0.1.0
    DESCRIPTION "Host-compiled unit tests for ra8-firmware"
)

# Host C23 build -- the target uses the same standard.
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
# Force the C++ standard into every C++ TU's compile command. Without this,
# CMake omits -std when clang's default already satisfies cxx_std_17, leaving
# the .cpp shims (ra_rabook_xml_shim, ra_epub_xml_shim, ...) with no -std in
# compile_commands.json -- then clang_tidy.sh's `--extra-arg-before=-std=c2x`
# is the only -std and clang rejects "-std=c2x not allowed with C++".
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
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

# RA_SANITIZE: when non-empty (e.g. "undefined"), instrument every host-test
# target with -fsanitize=<list> -- the undefined-behaviour gate driven by
# `make ubsan`. Independent of the coverage / MC-DC instrumentation above; pick
# at most one of {coverage, mcdc, sanitize} per build tree (separate dirs).
set(RA_SANITIZE "" CACHE STRING "Comma-separated -fsanitize list for host tests (e.g. undefined)")
if(RA_SANITIZE)
  add_compile_options(-fsanitize=${RA_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${RA_SANITIZE})
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
file(GLOB_RECURSE RA_IO_SOURCES      CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_io/src/*.c)
file(GLOB_RECURSE RA_FTL_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_ftl/src/*.c)
file(GLOB_RECURSE RA_MEM_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_mem/src/*.c)
file(GLOB_RECURSE RA_SDMMC_SPI_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_sdmmc_spi/src/*.c)
file(GLOB_RECURSE RA_GFX_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_gfx/src/*.c)
file(GLOB_RECURSE RA_UI_SOURCES      CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_ui/src/*.c)
file(GLOB_RECURSE RA_KEYBOARD_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_keyboard/src/*.c)
file(GLOB_RECURSE RA_BOX_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_box/src/*.c)
file(GLOB_RECURSE RA_BOOK_SOURCES    CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_book/src/*.c)
file(GLOB_RECURSE RA_RABOOK_COMPILE_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_rabook_compile/src/*.c)
file(GLOB_RECURSE RA_RABOOK_COMPILE_CPP_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_rabook_compile/src/*.cpp)
file(GLOB_RECURSE RA_RABOOK_IMPORT_SOURCES      CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_rabook_import/src/*.c)
file(GLOB_RECURSE RA_BATT_SOURCES    CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_batt/src/*.c)
file(GLOB_RECURSE RA_WIDGET_SOURCES  CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_widget/src/*.c)
file(GLOB_RECURSE RA_COMPOSITOR_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_compositor/src/*.c)
file(GLOB_RECURSE RA_APP_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_app/src/*.c)
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
    # The static stb_truetype arena allocator (ra_stbtt_malloc/free) is shared:
    # both v1 and the litehtml v2 engine rasterise glyphs through stb_truetype,
    # so this one file stays in even when the v1 reflow engine is excluded.
    # ra_stbtt_guard.c (the #217 sfnt table-directory bounds check) is likewise
    # shared: ra_epub's priv_font_init calls it before stbtt_InitFont, so its
    # symbol must resolve even under the v2 engine.
    # ra_img_arena.c (the stb_image bump arena) is likewise shared: the always-on
    # stb_image_impl.c TU in RA_EPUB_THIRD_PARTY references its symbols.
    # ra_reflow_link.c is pure query logic over engine fields (no layout/stbtt),
    # so the #110 link/anchor API is available under v2 as well.
    set(RA_REFLOW_C_SOURCES
        ${FW_ROOT}/libs/ra_reflow/src/ra_stbtt_alloc.c
        ${FW_ROOT}/libs/ra_reflow/src/ra_stbtt_guard.c
        ${FW_ROOT}/libs/ra_reflow/src/ra_img_arena.c
        ${FW_ROOT}/libs/ra_reflow/src/ra_reflow_link.c)
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
    ${FW_ROOT}/libs/third_party/stb/stb_image_impl.c
)
file(GLOB_RECURSE RA_SECURE_APP_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/src/secure_app/*.c)
file(GLOB_RECURSE RA_TOUCH_CAL_SOURCES  CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_touch_cal/src/*.c)
file(GLOB_RECURSE RA_PSA_CRYPTO_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_psa_crypto/src/*.c)
file(GLOB_RECURSE RA_WDT_SUPERVISOR_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_wdt_supervisor/src/*.c)
file(GLOB_RECURSE RA_MPU_SOURCES        CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_mpu/src/*.c)
file(GLOB_RECURSE RA_BOARD_EK_RA8D2_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_board_ek_ra8d2/src/*.c)
file(GLOB_RECURSE RA_LSM6DSO_SOURCES        CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_lsm6dso/src/*.c)
file(GLOB_RECURSE RA_TZ_SECURE_BOOT_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_tz_secure_boot/src/*.c)
file(GLOB_RECURSE RA_DFU_SOURCES        CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_dfu/src/*.c)

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
    ${RA_IO_SOURCES}
    ${RA_FTL_SOURCES}
    ${RA_MEM_SOURCES}
    ${RA_SDMMC_SPI_SOURCES}
    ${RA_GFX_SOURCES}
    ${RA_UI_SOURCES}
    ${RA_KEYBOARD_SOURCES}
    ${RA_BOX_SOURCES}
    ${RA_BOOK_SOURCES}
    ${RA_RABOOK_COMPILE_SOURCES}
    ${RA_RABOOK_COMPILE_CPP_SOURCES}
    ${RA_RABOOK_IMPORT_SOURCES}
    ${RA_BATT_SOURCES}
    ${RA_WIDGET_SOURCES}
    ${RA_COMPOSITOR_SOURCES}
    ${RA_APP_SOURCES}
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
    ${RA_DFU_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_mmap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_irq.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_dma.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_world.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_mmio.c
)
# STACK_USAGE_BYTES 0 disables the per-function stack gate for the host
# build (see the line 41 note): the host ABI pushes wider frames than the
# Cortex-M85, so -Wstack-usage here is a false-positive generator. The real
# target stack budget is enforced by the firmware build + stack_usage_check.py.
ra_target_enable_project_warnings(ra_core_hal STACK_USAGE_BYTES 0)
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
    ${FW_ROOT}/libs/ra_io/inc
    ${FW_ROOT}/libs/ra_ftl/inc
    ${FW_ROOT}/libs/ra_mem/inc
    ${FW_ROOT}/libs/ra_sdmmc_spi/inc
    ${FW_ROOT}/libs/ra_sdfont/inc
    ${FW_ROOT}/libs/ra_gfx/inc
    ${FW_ROOT}/libs/ra_ui/inc
    ${FW_ROOT}/libs/ra_keyboard/inc
    ${FW_ROOT}/libs/ra_box/inc
    ${FW_ROOT}/libs/ra_book/inc
    ${FW_ROOT}/libs/ra_rabook_compile/inc
    ${FW_ROOT}/libs/ra_rabook_import/inc
    ${FW_ROOT}/libs/ra_batt/inc
    ${FW_ROOT}/libs/ra_widget/inc
    ${FW_ROOT}/libs/ra_compositor/inc
    ${FW_ROOT}/libs/ra_app/inc
    ${FW_ROOT}/libs/ra_tls/inc
    ${FW_ROOT}/libs/ra_nsc/inc
    ${FW_ROOT}/libs/ra_ota/inc
    ${FW_ROOT}/libs/ra_dfu/inc
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
    ${FW_ROOT}/libs/ra_io/src
    ${FW_ROOT}/libs/ra_ftl/src
    ${FW_ROOT}/libs/ra_sdmmc_spi/src
    ${FW_ROOT}/libs/ra_gfx/src
    ${FW_ROOT}/libs/ra_nsc/src
    ${FW_ROOT}/libs/ra_ota/src
    ${FW_ROOT}/libs/ra_dfu/src
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
    ${RA_RABOOK_COMPILE_CPP_SOURCES}
    ${RA_REFLOW_CPP_SOURCES}
    PROPERTIES LANGUAGE CXX
)
# -w silences the vendored decoders' warnings; -fno-strict-aliasing matches the
# app build (cmake/ra_add_app.cmake) -- these decoders type-pun through byte
# buffers (arm-gcc miscompiles miniz's inflate under strict aliasing at -Og/-O2).
# The host-test build previously avoided the same UB only by incidentally running
# at -O0; set it structurally here too so a new vendored decoder, or a host build
# raised to -Og, cannot resurface it (T5-16).
set_source_files_properties(
    ${RA_EPUB_THIRD_PARTY}
    PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing"
)

# Route stb_truetype's per-glyph scratch (vertices, rasteriser edge/point
# lists) through the no-heap arena in libs/ra_reflow/src/ra_stbtt_alloc.c
# instead of libc malloc, so the host test build exercises the exact
# heap-free path the firmware uses. The -include makes the allocator
# declarations visible inside the vendored single-TU implementation
# without modifying SOUP. (Appends to the -w set above.)
set_property(SOURCE ${FW_ROOT}/libs/third_party/stb/stb_truetype_impl.c
    APPEND PROPERTY COMPILE_OPTIONS
    -include "${FW_ROOT}/libs/ra_reflow/inc/ra_stbtt_alloc.h"
    "-DSTBTT_malloc(x,u)=ra_stbtt_malloc(x)"
    "-DSTBTT_free(x,u)=ra_stbtt_free(x)"
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

# ---------------------------------------------------------------------------
# ra_core_hal_fuzz: slim mirror of ra_core_hal for the libFuzzer harnesses
# (RA_FUZZ=ON only; see tests/fuzz/CMakeLists.txt).
#
# ra_core_hal bundles tests/mocks/ra_sim_mmap.c, whose load-time MAP_FIXED
# constructor maps a peripheral backing window at 0xE0000000. Under
# AddressSanitizer (the Linux fuzz config) that address is inside ASan's
# reserved shadow gap, so the constructor aborts every harness before
# main() (issue #193). This object library is exactly ra_core_hal MINUS
# ra_sim_mmap.c: the pure-computation harnesses link it and never run that
# constructor. The two harnesses that genuinely poke peripheral registers
# (fuzz_ra_canfd, fuzz_ra_usb_pal) add ra_sim_mmap.c back on their own link
# line, where it is compiled under ASan and skips the shadow-gap region
# (see ra_sim_region_mappable() in tests/mocks/ra_sim_mmap.c). ra_core_hal
# itself is untouched, so the host unit-test build keeps the full mapping.
#
# Sources and per-source properties (LANGUAGE, -w, MC/DC opt-out, the
# stb_truetype -include) are shared with ra_core_hal because
# set_source_files_properties() is directory-scoped: both libraries pick
# them up from the same source paths. Only built when RA_FUZZ=ON, so the
# ordinary `make test` build never compiles this second object set.
# ---------------------------------------------------------------------------
option(RA_FUZZ "Build libFuzzer harnesses for parsers (clang only)" OFF)
if(RA_FUZZ)
  get_target_property(_ra_core_hal_srcs ra_core_hal SOURCES)
  list(REMOVE_ITEM _ra_core_hal_srcs
       ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_mmap.c)
  add_library(ra_core_hal_fuzz OBJECT ${_ra_core_hal_srcs})
  ra_target_enable_project_warnings(ra_core_hal_fuzz STACK_USAGE_BYTES 0)
  target_compile_options(ra_core_hal_fuzz PRIVATE
      -Wno-unused-function
      -Wno-unused-parameter
      -Wno-unused-variable
      -Wno-address-of-packed-member
  )
  target_include_directories(ra_core_hal_fuzz PUBLIC
      $<TARGET_PROPERTY:ra_core_hal,INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_features(ra_core_hal_fuzz PUBLIC cxx_std_17)
  if(RA_REFLOW_USE_LITEHTML)
    target_link_libraries(ra_core_hal_fuzz PUBLIC litehtml gumbo)
  endif()
endif()

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
    ${FW_ROOT}/libs/ra_io/inc
    ${FW_ROOT}/libs/ra_ftl/inc
    ${FW_ROOT}/libs/ra_mem/inc
    ${FW_ROOT}/libs/ra_sdmmc_spi/inc
    ${FW_ROOT}/libs/ra_sdfont/inc
    ${FW_ROOT}/libs/ra_gfx/inc
    ${FW_ROOT}/libs/ra_ui/inc
    ${FW_ROOT}/libs/ra_keyboard/inc
    ${FW_ROOT}/libs/ra_box/inc
    ${FW_ROOT}/libs/ra_book/inc
    ${FW_ROOT}/libs/ra_rabook_compile/inc
    ${FW_ROOT}/libs/ra_rabook_import/inc
    ${FW_ROOT}/libs/ra_batt/inc
    ${FW_ROOT}/libs/ra_widget/inc
    ${FW_ROOT}/libs/ra_compositor/inc
    ${FW_ROOT}/libs/ra_app/inc
    ${FW_ROOT}/libs/ra_tls/inc
    ${FW_ROOT}/libs/ra_nsc/inc
    ${FW_ROOT}/libs/ra_ota/inc
    ${FW_ROOT}/libs/ra_dfu/inc
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
    ${FW_ROOT}/libs/ra_io/src
    ${FW_ROOT}/libs/ra_ftl/src
    ${FW_ROOT}/libs/ra_sdmmc_spi/src
    ${FW_ROOT}/libs/ra_gfx/src
    ${FW_ROOT}/libs/ra_nsc/src
    ${FW_ROOT}/libs/ra_ota/src
    ${FW_ROOT}/libs/ra_dfu/src
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
#  if(APPLE)
#    target_link_options(${name} PRIVATE
#      "-Wl,-pagezero_size,0x4000"
#      "-Wl,-segalign,0x4000"
#    )
#  endif()
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

# test_psa_real_kat.c links the REAL crypto backend (TF-PSA-Crypto), not the
# sim'd ra_core_hal, so it is registered by hand below rather than through the
# ra_add_test() auto-glob (which would splice in the RA_SIMULATOR_MODE build).
list(REMOVE_ITEM RA_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_psa_real_kat.c)

# test_ra_cache_store.c needs the vendored LevelX NOR sources + a RAM NOR
# simulator + LX_STANDALONE_ENABLE, so it is registered by hand below rather
# than through the ra_add_test() auto-glob.
list(REMOVE_ITEM RA_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_ra_cache_store.c)

# test_ra_rsip_devsec_failclosed.c (issue #216) must compile ra_rsip_devsec.c
# with the stub-crypto guard flags UNDEFINED so the production fail-closed #else
# is the body under test. The rest of the host build force-defines
# RA_SIMULATOR_MODE, so it is registered by hand below rather than through the
# ra_add_test() auto-glob (which would splice in the RA_SIMULATOR_MODE build).
list(REMOVE_ITEM RA_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_ra_rsip_devsec_failclosed.c)

foreach(src ${RA_TEST_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  ra_add_test(${name})
endforeach()

# ---------------------------------------------------------------------------
# test_stub_crypto_gate (issue #180): reachability half of the stub-vs-production
# crypto gate. The auto-glob above already registered it (it links ra_core_hal,
# built under RA_SIMULATOR_MODE, so the guarded #if branch is active). Define
# RA_INSECURE_STUB_CRYPTO on it too, so it faithfully compiles under the opt-in
# flag the gate governs -- both flags select the same insecure #if branch, and
# the test asserts those stub bodies are reachable-but-insecure (a fail-closed
# #else would return k_ra_err_not_supported and fail every case).
# ---------------------------------------------------------------------------
if(TARGET test_stub_crypto_gate)
  target_compile_definitions(test_stub_crypto_gate PRIVATE RA_INSECURE_STUB_CRYPTO)
endif()

# ---------------------------------------------------------------------------
# test_ra_rsip_devsec_failclosed (issue #216): production fail-closed reachability
# for the RSIP device-security path (lifecycle / debug / tamper / DPA arm). The
# rest of the host build compiles ra_rsip_devsec.c under RA_SIMULATOR_MODE (the
# guarded #if branch, exercised by test_life / test_debug_level / test_tamper in
# test_ra_rsip.c). This self-contained target (no ra_core_hal, no sim) rebuilds
# JUST that TU with the two guard flags UNDEFINED so the production #else is the
# compiled body, and asserts every entry point returns k_ra_err_not_supported and
# writes no fabricated state. -U wins over the directory-level -D because CMake
# emits DEFINES before FLAGS on the compile line. -DRA_LOG_LEVEL=0 makes the
# RA_CHECK_NULL_PTR log a no-op so the TU needs no ra_log backend to link.
# ---------------------------------------------------------------------------
add_executable(test_ra_rsip_devsec_failclosed
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra_rsip_devsec_failclosed.c
  ${FW_ROOT}/libs/ra_hal/src/ra_rsip_devsec.c)
target_include_directories(test_ra_rsip_devsec_failclosed PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${FW_ROOT}/libs/ra_core/inc
  ${FW_ROOT}/libs/ra_hal/inc)
target_compile_options(test_ra_rsip_devsec_failclosed PRIVATE
  -Wall -Wextra -Werror
  -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable
  -URA_SIMULATOR_MODE -URA_INSECURE_STUB_CRYPTO -DRA_LOG_LEVEL=0)
add_test(NAME test_ra_rsip_devsec_failclosed COMMAND test_ra_rsip_devsec_failclosed)

# ---------------------------------------------------------------------------
# T5-04: real-backend crypto known-answer test. The rest of the host build
# compiles ra_psa_crypto.c under RA_SIMULATOR_MODE (a deterministic AES-GCM
# stand-in + an HMAC-tautology ECDSA "verify"), so the AEAD / signature paths
# are never checked against a real cipher. This target links the vendored
# TF-PSA-Crypto library directly -- the exact psa_* primitives the production
# !RA_SIMULATOR_MODE path calls -- and pins them to NIST/RFC vectors. It is a
# self-contained executable (no ra_core_hal, no sim). TF-PSA-Crypto is SOUP:
# compile it -w -fno-strict-aliasing like the other vendored trees; it is
# already excluded from the coverage filter (libs/third_party/).
set(_RA_TFPSA_KAT_DIR ${FW_ROOT}/libs/third_party/tf-psa-crypto)
file(GLOB _RA_TFPSA_KAT_SRC CONFIGURE_DEPENDS
  ${_RA_TFPSA_KAT_DIR}/core/*.c
  ${_RA_TFPSA_KAT_DIR}/drivers/builtin/src/*.c
  ${_RA_TFPSA_KAT_DIR}/platform/*.c
  ${_RA_TFPSA_KAT_DIR}/utilities/*.c
  ${_RA_TFPSA_KAT_DIR}/extras/*.c)
set(_RA_TFPSA_KAT_INC
  ${_RA_TFPSA_KAT_DIR}/include
  ${_RA_TFPSA_KAT_DIR}/drivers/builtin/include
  ${_RA_TFPSA_KAT_DIR}/core
  ${_RA_TFPSA_KAT_DIR}/dispatch
  ${_RA_TFPSA_KAT_DIR}/drivers/builtin/src
  ${_RA_TFPSA_KAT_DIR}/platform
  ${_RA_TFPSA_KAT_DIR}/utilities
  ${_RA_TFPSA_KAT_DIR}/extras)
add_library(ra_tfpsa_host_kat OBJECT ${_RA_TFPSA_KAT_SRC})
target_include_directories(ra_tfpsa_host_kat PRIVATE ${_RA_TFPSA_KAT_INC})
target_compile_options(ra_tfpsa_host_kat PRIVATE -w -fno-strict-aliasing)
add_executable(test_psa_real_kat
  ${CMAKE_CURRENT_SOURCE_DIR}/test_psa_real_kat.c
  $<TARGET_OBJECTS:ra_tfpsa_host_kat>)
target_include_directories(test_psa_real_kat PRIVATE ${_RA_TFPSA_KAT_INC})
target_compile_options(test_psa_real_kat PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_psa_real_kat COMMAND test_psa_real_kat)

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
#  if(APPLE)
#    target_link_options(test_ra_reflow_v2 PRIVATE
#      "-Wl,-pagezero_size,0x4000"
#      "-Wl,-segalign,0x4000"
#    )
#  endif()
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
#if(APPLE)
#  target_link_options(test_ra_epub_xml_shim PRIVATE
#    "-Wl,-pagezero_size,0x4000"
#    "-Wl,-segalign,0x4000"
#  )
#endif()
add_test(NAME test_ra_epub_xml_shim COMMAND test_ra_epub_xml_shim)

# ---------------------------------------------------------------------------
# White-box line-coverage target for ra_epub_xml_shim. This test #includes the
# shim source with its four exported symbols renamed (*_cov) so the anonymous-
# namespace helpers can be driven directly with the edge inputs the public API
# filters out. It adds libs/ra_epub/src to the include path so the
# `#include "ra_epub_xml_shim.cpp"` resolves; tinyxml2 links from ra_core_hal.
# ---------------------------------------------------------------------------
add_executable(test_ra_epub_xml_shim_cov
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra_epub_xml_shim_cov.cpp
  $<TARGET_OBJECTS:ra_core_hal>
)
set_target_properties(test_ra_epub_xml_shim_cov PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_ra_epub_xml_shim_cov PRIVATE
  -Wall -Wextra -Wno-unused-function -Wno-unused-parameter
  -Wno-unused-variable -Wno-address-of-packed-member
)
target_compile_features(test_ra_epub_xml_shim_cov PRIVATE cxx_std_17)
target_include_directories(test_ra_epub_xml_shim_cov PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${FW_ROOT}/libs/ra_core/inc
  ${FW_ROOT}/libs/ra_epub/inc
  ${FW_ROOT}/libs/ra_epub/src
  ${FW_ROOT}/libs/third_party/tinyxml2
  ${FW_ROOT}/libs/third_party/stb
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
if(RA_REFLOW_USE_LITEHTML)
  target_link_libraries(test_ra_epub_xml_shim_cov PRIVATE litehtml gumbo)
endif()
add_test(NAME test_ra_epub_xml_shim_cov COMMAND test_ra_epub_xml_shim_cov)

# ---------------------------------------------------------------------------
# Complementary line-coverage target for ra_epub_xml_shim. Unlike the _cov
# sibling, this test is BLACK-BOX: it links the real (un-renamed) production
# entry points from ra_core_hal and drives their length / parse / root /
# rootfile-resolution / spine-skip / cap / cover-meta / deep-recurse / nav-skip
# legs with fixtures. Calling the un-renamed functions lands the entry-point
# body lines in the shared production object so gcovr merges them into the
# aggregate (a renamed white-box copy would attribute them to a distinct
# function and drop the union). No source #include; tinyxml2 links transitively.
# ---------------------------------------------------------------------------
add_executable(test_ra_epub_xml_shim_cov2
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra_epub_xml_shim_cov2.cpp
  $<TARGET_OBJECTS:ra_core_hal>
)
set_target_properties(test_ra_epub_xml_shim_cov2 PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_ra_epub_xml_shim_cov2 PRIVATE
  -Wall -Wextra -Wno-unused-function -Wno-unused-parameter
  -Wno-unused-variable -Wno-address-of-packed-member
)
target_compile_features(test_ra_epub_xml_shim_cov2 PRIVATE cxx_std_17)
target_include_directories(test_ra_epub_xml_shim_cov2 PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${FW_ROOT}/libs/ra_core/inc
  ${FW_ROOT}/libs/ra_epub/inc
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
if(RA_REFLOW_USE_LITEHTML)
  target_link_libraries(test_ra_epub_xml_shim_cov2 PRIVATE litehtml gumbo)
endif()
add_test(NAME test_ra_epub_xml_shim_cov2 COMMAND test_ra_epub_xml_shim_cov2)

# ---------------------------------------------------------------------------
# C++ test target for ra_rabook_xml_shim. Drives the public C-linkage entry
# point ra_rabook_xml_parse_chapter() to verify DOM pre-order and MC/DC
# vectors for the iterative DFS (#149 stage-a XHTML parser).
# ---------------------------------------------------------------------------
add_executable(test_ra_rabook_xml_shim
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra_rabook_xml_shim.cpp
  $<TARGET_OBJECTS:ra_core_hal>
)
set_target_properties(test_ra_rabook_xml_shim PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_ra_rabook_xml_shim PRIVATE
  -Wall -Wextra -Wno-unused-function -Wno-unused-parameter
  -Wno-unused-variable -Wno-address-of-packed-member
)
target_compile_features(test_ra_rabook_xml_shim PRIVATE cxx_std_17)
target_include_directories(test_ra_rabook_xml_shim PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${FW_ROOT}/libs/ra_core/inc
  ${FW_ROOT}/libs/ra_book/inc
  ${FW_ROOT}/libs/ra_rabook_compile/inc
  ${FW_ROOT}/libs/third_party/tinyxml2
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
if(RA_REFLOW_USE_LITEHTML)
  target_link_libraries(test_ra_rabook_xml_shim PRIVATE litehtml gumbo)
endif()
add_test(NAME test_ra_rabook_xml_shim COMMAND test_ra_rabook_xml_shim)

# ---------------------------------------------------------------------------
# exFAT host test (#85 read support / #93 leading-slash open regression).
# Standalone: links ONLY ra_fs_fat.c (no ra_core_hal -> no ra_time
# weak-extern), so it builds and runs on macOS and Linux alike. The fixture
# is a tiny real exFAT image checked in gzipped; it is decompressed to the
# build dir at build time and its path injected via RA_EXFAT_FIXTURE.
# ---------------------------------------------------------------------------
set(RA_EXFAT_FIXTURE_GZ ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ra_fs/exfat_small.img.gz)
set(RA_EXFAT_FIXTURE_IMG ${CMAKE_CURRENT_BINARY_DIR}/exfat_small.img)
add_custom_command(
  OUTPUT ${RA_EXFAT_FIXTURE_IMG}
  COMMAND bash -c "gunzip -c '${RA_EXFAT_FIXTURE_GZ}' > '${RA_EXFAT_FIXTURE_IMG}'"
  DEPENDS ${RA_EXFAT_FIXTURE_GZ}
  COMMENT "Decompressing exFAT test fixture"
  VERBATIM
)
add_custom_target(ra_exfat_fixture DEPENDS ${RA_EXFAT_FIXTURE_IMG})
# ra_fs_fat.c was split into ra_fs_fat*.c TUs for the 1000-line file-size cap;
# glob them all so this standalone test still links the whole FAT driver (and
# only the FAT driver -- no ra_core_hal -> no ra_time weak-extern).
file(GLOB RA_FS_FAT_TU_SOURCES CONFIGURE_DEPENDS
  ${FW_ROOT}/libs/ra_fs/src/ra_fs_fat*.c)
add_executable(test_ra_fs_exfat
  ${CMAKE_CURRENT_SOURCE_DIR}/host/exfat_fs_test.c
  ${RA_FS_FAT_TU_SOURCES}
)
add_dependencies(test_ra_fs_exfat ra_exfat_fixture)
target_compile_options(test_ra_fs_exfat PRIVATE -Wall -Wextra)
target_include_directories(test_ra_fs_exfat PRIVATE
  ${FW_ROOT}/libs/ra_fs/inc
  ${FW_ROOT}/libs/ra_core/inc
  ${FW_ROOT}/libs/ra_hal/inc
)
target_compile_definitions(test_ra_fs_exfat PRIVATE RA_EXFAT_FIXTURE="${RA_EXFAT_FIXTURE_IMG}")
add_test(NAME test_ra_fs_exfat COMMAND test_ra_fs_exfat)

# ---------------------------------------------------------------------------
# test_ra_cache_store (#201): persistent key->blob cache over LevelX standalone.
# Builds ra_cache_store's own sources plus the vendored LevelX NOR sources
# (LX_STANDALONE_ENABLE, no ThreadX) and the RAM NOR simulator mock. ra_core_hal
# supplies ra_log / ra_err / ra_vsource. LevelX is SOUP: compile it -w and it is
# already outside the coverage filter (libs/third_party/).
# ---------------------------------------------------------------------------
file(GLOB RA_CACHE_STORE_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_cache_store/src/*.c)
file(GLOB RA_LEVELX_NOR_STANDALONE CONFIGURE_DEPENDS
  ${FW_ROOT}/libs/third_party/levelx/common/src/lx_nor_*.c)
# Drop the upstream NOR simulator: this test supplies its own RAM NOR driver.
list(FILTER RA_LEVELX_NOR_STANDALONE EXCLUDE REGEX ".*/lx_nor_flash_simulator\\.c$")
add_executable(test_ra_cache_store
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra_cache_store.c
  ${RA_CACHE_STORE_SOURCES}
  ${RA_LEVELX_NOR_STANDALONE}
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/lx_nor_sim_ram.c
  $<TARGET_OBJECTS:ra_core_hal>
)
set_target_properties(test_ra_cache_store PROPERTIES LINKER_LANGUAGE CXX)
target_compile_definitions(test_ra_cache_store PRIVATE LX_STANDALONE_ENABLE)
target_compile_options(test_ra_cache_store PRIVATE
  -Wall -Wextra -Wno-unused-function -Wno-unused-parameter)
target_include_directories(test_ra_cache_store PRIVATE
  ${FW_ROOT}/libs/ra_cache_store/inc
  ${FW_ROOT}/libs/ra_cache_store/src
  ${FW_ROOT}/libs/ra_core/inc
  ${FW_ROOT}/libs/ra_hal/inc
  ${FW_ROOT}/libs/ra_mem/inc
  ${FW_ROOT}/libs/third_party/levelx/common/inc
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
# LevelX SOUP: silence its warnings (matches cmake/levelx.cmake handling).
set_source_files_properties(${RA_LEVELX_NOR_STANDALONE} PROPERTIES COMPILE_OPTIONS "-w")
add_test(NAME test_ra_cache_store COMMAND test_ra_cache_store)

# ---------------------------------------------------------------------------
# Optional libFuzzer harnesses (opt-in via -DRA_FUZZ=ON; clang-only).
# Each fuzz target is built as a separate executable that links against
# the existing ra_core_hal OBJECT library and adds
# -fsanitize=fuzzer,address,undefined only on its own translation unit.
# Default OFF so the normal host test build is unaffected.
# See docs/FUZZING.md. The RA_FUZZ option and the slim ra_core_hal_fuzz
# object library it gates are declared earlier, next to ra_core_hal.
# ---------------------------------------------------------------------------
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

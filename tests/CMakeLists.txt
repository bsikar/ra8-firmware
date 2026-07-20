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

project(
  ra8-firmware-tests
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
# the .cpp shims (ra8_rabook_xml_shim, ra8_epub_xml_shim, ...) with no -std in
# compile_commands.json -- then clang_tidy.sh's `--extra-arg-before=-std=c2x`
# is the only -std and clang rejects "-std=c2x not allowed with C++".
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ra8_warnings.cmake)

# Every TU is compiled with RA8_SIMULATOR_MODE so code that normally
# reaches into MCU register blocks takes the host-safe path instead.
add_compile_definitions(RA8_SIMULATOR_MODE UNIT_TEST)

# Host warnings are applied per target below. We deliberately do NOT use
# -Wstack-usage=2200 here because the host compiler ABI pushes wider
# arguments than Cortex-M85.

# Coverage instrumentation -- gcovr / lcov friendly.
option(RA8_COVERAGE "Emit gcov/lcov coverage data" ON)
option(RA8_MCDC "Emit clang source-based coverage with MC/DC (DO-178C Level B)" OFF)

# RA8_MCDC takes precedence over RA8_COVERAGE: the two instrumentation
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
if(RA8_MCDC)
  set(RA8_COVERAGE
      OFF
      CACHE BOOL "Disabled because RA8_MCDC=ON" FORCE
  )

  # Probe: does the active C compiler accept the MC/DC flag trio?
  #
  # Drop any cached answer first (#346). check_c_compiler_flag stores its
  # result in the CACHE and skips the check entirely when that entry already
  # exists -- but the answer is a property of the COMPILER, not of the build
  # dir. Re-configuring an existing tree with a different compiler therefore
  # reuses the previous compiler's verdict: tests/build-cov configured by the
  # coverage gate with gcc-13 (no -fcoverage-mcdc) cached "no", and the mcdc
  # gate's later clang-18 configure silently inherited it, built with plain
  # --coverage, and emitted no .profraw at all while reporting MC/DC support.
  # Re-probing costs about a second; trusting a stale answer cost a gate that
  # could not measure the thing it exists to measure.
  unset(RA8_HAVE_CLANG_MCDC CACHE)
  unset(RA8_HAVE_GCC_CONDCOV CACHE)
  include(CheckCCompilerFlag)
  set(CMAKE_REQUIRED_FLAGS "-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc")
  check_c_compiler_flag("-fcoverage-mcdc" RA8_HAVE_CLANG_MCDC)
  unset(CMAKE_REQUIRED_FLAGS)

  if(RA8_HAVE_CLANG_MCDC)
    message(STATUS "RA8_MCDC: clang MC/DC instrumentation ENABLED "
                   "(-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc)"
    )
    set(RA8_MCDC_C_FLAGS
        -O0
        -g
        -fprofile-instr-generate
        -fcoverage-mapping
        -fcoverage-mcdc
    )
    add_link_options(-fprofile-instr-generate -fcoverage-mapping)
  else()
    # gcc 14+ ships -fcondition-coverage. Probe for it as a best-effort
    # fallback. This is NOT MC/DC -- it is condition coverage only.
    set(CMAKE_REQUIRED_FLAGS "-fprofile-arcs -ftest-coverage -fcondition-coverage")
    check_c_compiler_flag("-fcondition-coverage" RA8_HAVE_GCC_CONDCOV)
    unset(CMAKE_REQUIRED_FLAGS)

    # Degrading here is what turned a one-line configure problem into a
    # misleading failure four steps downstream (#346): the build came out
    # uninstrumented, every test passed, and the gate then blamed the tests
    # for "crashing" because no .profraw existed. RA8_MCDC=ON is an explicit
    # request to measure MC/DC; a toolchain that cannot must say so HERE.
    #
    # RA8_MCDC_ALLOW_FALLBACK=ON opts into the non-MC/DC fallback for local
    # exploration on a gcc-only box. It is deliberately not the default and
    # must never be set on a gate path -- the resulting report contains no
    # condition-level data and cannot support a DO-178C Level B claim.
    if(NOT RA8_MCDC_ALLOW_FALLBACK)
      message(
        FATAL_ERROR
          "RA8_MCDC=ON but ${CMAKE_C_COMPILER} does not support clang's "
          "-fcoverage-mcdc. DO-178C Level B MC/DC CANNOT be measured with "
          "this toolchain, so this configure fails rather than silently "
          "producing an uninstrumented build that yields no .profraw. "
          "Install clang >= 18 (brew install llvm / apt install clang-18), "
          "or pass -DRA8_MCDC_ALLOW_FALLBACK=ON to accept plain "
          "decision/condition coverage instead (NOT MC/DC)."
      )
    endif()

    message(
      WARNING "RA8_MCDC requested but the host compiler does not support "
              "clang's -fcoverage-mcdc. DO-178C Level B MC/DC CANNOT be "
              "measured with this toolchain. "
              "Install clang >= 18 (brew install llvm / apt install clang-18) "
              "and re-run."
    )

    if(RA8_HAVE_GCC_CONDCOV)
      message(WARNING "Falling back to gcc -fcondition-coverage (condition coverage "
                      "only, NOT MC/DC)."
      )
      set(RA8_MCDC_C_FLAGS
          -O0
          -g
          --coverage
          -fprofile-arcs
          -ftest-coverage
          -fcondition-coverage
      )
      add_link_options(--coverage -fprofile-arcs -ftest-coverage)
    else()
      message(WARNING "Falling back to plain gcov decision/branch coverage. No "
                      "condition-level data will be produced."
      )
      set(RA8_MCDC_C_FLAGS
          -O0
          -g
          --coverage
          -fprofile-arcs
          -ftest-coverage
      )
      add_link_options(--coverage -fprofile-arcs -ftest-coverage)
    endif()
  endif()

  # Apply globally so test executables get the same flags as the
  # ra8_core_hal OBJECT library. Per-source opt-out for third_party
  # is performed below, after the source lists are populated.
  add_compile_options(${RA8_MCDC_C_FLAGS})
elseif(RA8_COVERAGE)
  add_compile_options(
    -O0
    -g
    --coverage
    -fprofile-arcs
    -ftest-coverage
  )
  add_link_options(--coverage -fprofile-arcs -ftest-coverage)
endif()

# Compiler cache. Included AFTER the coverage / MC-DC options are resolved
# above, because it opts out when either is on -- gcov and clang profile data
# embed absolute paths that a cached object would carry into the wrong build
# directory. See cmake/ccache.cmake for the full rationale.
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ccache.cmake")

# RA8_SANITIZE: when non-empty (e.g. "undefined"), instrument every host-test
# target with -fsanitize=<list> -- the undefined-behaviour gate driven by
# `make ubsan`. Independent of the coverage / MC-DC instrumentation above; pick
# at most one of {coverage, mcdc, sanitize} per build tree (separate dirs).
set(RA8_SANITIZE
    ""
    CACHE STRING "Comma-separated -fsanitize list for host tests (e.g. undefined)"
)
if(RA8_SANITIZE)
  add_compile_options(-fsanitize=${RA8_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${RA8_SANITIZE})
  # UBSan reserves no low-memory shadow (unlike ASan), so it can still take the
  # image pin below that keeps text/.data/.bss/brk clear of the MAP_FIXED
  # peripheral windows. Without it a ~40 MiB test image -- the 32 MiB XSPI sim
  # backing bundled into ra8_core_hal dominates every test's .bss -- loads at
  # 0x400000 and its .bss crosses the code-MRAM window at 0x02000000, so
  # ra8_sim_mmap_install aborts before main(). Apply the pin for every sanitizer
  # except address, which owns its own low-address layout and skips the
  # shadow-gap window at runtime instead.
  if(NOT RA8_SANITIZE MATCHES "address")
    add_link_options(-no-pie -Wl,-Ttext-segment=0x70000000)
  endif()
else()
  # The MAP_FIXED peripheral windows span [0x02000000, 0x68100000) (see
  # tests/mocks/ra8_sim_mmap.c). On toolchains that default to non-PIE the
  # image loads at 0x400000, so a large-enough .bss (the gcov counters of a
  # coverage build crossed 0x02000000 in practice) pushes static data and the
  # brk heap INTO a window; the constructor's mmap then silently replaces live
  # pages and glibc aborts on the first heap extension. Pin the image above
  # every window so text/.data/.bss/brk can never intersect one. AddressSanitizer
  # is excluded above: it owns its low layout and skips the shadow-gap window
  # instead (ra8_sim_mmap_install still checks overlap at runtime).
  add_link_options(-no-pie -Wl,-Ttext-segment=0x70000000)
endif()

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

add_library(
  ra8_core_hal OBJECT
  ${RA8_CORE_SOURCES}
  ${RA8_HAL_SOURCES}
  ${RA8_NET_PAL_SOURCES}
  ${RA8_MODEM_AT_SOURCES}
  ${RA8_BLE_HOST_SOURCES}
  ${RA8_TLS_SOURCES}
  ${RA8_USB_PAL_SOURCES}
  ${RA8_FS_SOURCES}
  ${RA8_IO_SOURCES}
  ${RA8_FTL_SOURCES}
  ${RA8_MEM_SOURCES}
  ${RA8_SDMMC_SPI_SOURCES}
  ${RA8_GFX_SOURCES}
  ${RA8_UI_SOURCES}
  ${RA8_KEYBOARD_SOURCES}
  ${RA8_BOX_SOURCES}
  ${RA8_BOOK_SOURCES}
  ${RA8_RABOOK_COMPILE_SOURCES}
  ${RA8_RABOOK_COMPILE_CPP_SOURCES}
  ${RA8_RABOOK_IMPORT_SOURCES}
  ${RA8_BATT_SOURCES}
  ${RA8_WIDGET_SOURCES}
  ${RA8_APP_SOURCES}
  ${RA8_NSC_SOURCES}
  ${RA8_OTA_SOURCES}
  ${RA8_DISPLAY_PAL_SOURCES}
  ${RA8_POWER_PROFILE_SOURCES}
  ${RA8_EPUB_C_SOURCES}
  ${RA8_EPUB_CPP_SOURCES}
  ${RA8_COMIC_SOURCES}
  ${RA8_UNARCH_SOURCES}
  ${RA8_JOF_SOURCES}
  ${RA8_LONGSTRIP_SOURCES}
  ${RA8_REFLOW_C_SOURCES}
  ${RA8_REFLOW_CPP_SOURCES}
  ${RA8_EPUB_THIRD_PARTY}
  ${RA8_XZ_THIRD_PARTY}
  ${RA8_WEBP_THIRD_PARTY}
  ${RA8_WEBP_SOURCES}
  ${RA8_SECURE_APP_SOURCES}
  ${RA8_TOUCH_CAL_SOURCES}
  ${RA8_EPD_CAL_SOURCES}
  ${RA8_MPU_SOURCES}
  ${RA8_PSA_CRYPTO_SOURCES}
  ${RA8_WDT_SUPERVISOR_SOURCES}
  ${RA8_BOARD_EK_RA8D2_SOURCES}
  ${RA8_LSM6DSO_SOURCES}
  ${RA8_TZ_SECURE_BOOT_SOURCES}
  ${RA8_DFU_SOURCES}
  # ThreadX SysTick retune (issue #287). Lives under port/threadx (not
  # libs/), so it is not caught by the libs/ globs above -- add it by
  # hand. Its SYST_RVR/CVR writes compile out under RA8_SIMULATOR_MODE,
  # so the host build exercises only the clock-query + reload arithmetic.
  ${FW_ROOT}/port/threadx/src/cortex_m85/tx_systick_retune.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra8_sim_mmap.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra8_sim_irq.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra8_sim_dma.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra8_sim_time.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra8_sim_world.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra8_sim_mmio.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra8_sim_xspi_flash.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra8_host_asm_stub.c
)
# STACK_USAGE_BYTES 0 disables the per-function stack gate for the host
# build (see the line 41 note): the host ABI pushes wider frames than the
# Cortex-M85, so -Wstack-usage here is a false-positive generator. The real
# target stack budget is enforced by the firmware build + stack_usage_check.py.
ra8_target_enable_project_warnings(ra8_core_hal STACK_USAGE_BYTES 0)
target_compile_options(
  ra8_core_hal PRIVATE -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable
                       -Wno-address-of-packed-member
)
target_include_directories(
  ra8_core_hal
  PUBLIC ${FW_ROOT}/libs/ra8_core/inc
         ${FW_ROOT}/libs/ra8_hal/inc
         ${FW_ROOT}/libs/ra8_net_pal/inc
         ${FW_ROOT}/libs/ra8_modem_at/inc
         ${FW_ROOT}/libs/ra8_ble_host/inc
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
         ${FW_ROOT}/libs/ra8_jof/inc
         ${FW_ROOT}/libs/ra8_longstrip/inc
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
         ${FW_ROOT}/libs/third_party/xz_embedded
         # libwebp is included as "src/webp/...", so its ROOT is the include dir.
         ${FW_ROOT}/libs/third_party/libwebp
         ${FW_ROOT}/libs/ra8_webp/inc
         ${FW_ROOT}/libs/ra8_webp/src
         ${FW_ROOT}/src
         ${FW_ROOT}/src/inc
         ${FW_ROOT}/src/secure_app
         ${FW_ROOT}/src/secure_app/inc
         # Per-module src/ directories exposed for MC/DC test access to
         # internal helpers (see CLAUDE.md "Test access to internal symbols").
         # Tests under tests/ MAY include "<module>_internal.h" to drive MC/DC
         # vectors on production source text. Only first-party libs are listed.
         ${FW_ROOT}/libs/ra8_core/src
         ${FW_ROOT}/libs/ra8_hal/src
         ${FW_ROOT}/libs/ra8_net_pal/src
         ${FW_ROOT}/libs/ra8_modem_at/src
         ${FW_ROOT}/libs/ra8_ble_host/src
         ${FW_ROOT}/libs/ra8_tls/src
         ${FW_ROOT}/libs/ra8_usb_pal/src
         ${FW_ROOT}/libs/ra8_fs/src
         ${FW_ROOT}/libs/ra8_io/src
         ${FW_ROOT}/libs/ra8_ftl/src
         ${FW_ROOT}/libs/ra8_sdmmc_spi/src
         ${FW_ROOT}/libs/ra8_gfx/src
         ${FW_ROOT}/libs/ra8_nsc/src
         ${FW_ROOT}/libs/ra8_ota/src
         ${FW_ROOT}/libs/ra8_dfu/src
         ${FW_ROOT}/libs/ra8_display_pal/src
         ${FW_ROOT}/libs/ra8_power_profile/src
         ${FW_ROOT}/libs/ra8_touch_cal/src
         ${FW_ROOT}/libs/ra8_epd_cal/src
         ${FW_ROOT}/libs/ra8_mpu/src
         ${FW_ROOT}/libs/ra8_psa_crypto/src
         ${FW_ROOT}/libs/ra8_wdt_supervisor/src
         ${FW_ROOT}/libs/ra8_board_ek_ra8d2/src
         ${FW_ROOT}/libs/ra8_lsm6dso/src
         ${FW_ROOT}/libs/ra8_epub/src
         ${FW_ROOT}/libs/ra8_comic/src
         ${FW_ROOT}/libs/ra8_unarch/src
         ${FW_ROOT}/libs/ra8_jof/src
         ${FW_ROOT}/libs/ra8_longstrip/src
         ${FW_ROOT}/libs/ra8_reflow/src
         ${FW_ROOT}/libs/ra8_tz_secure_boot/src
         ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
# tinyxml2.cpp + ra8_epub_xml_shim.cpp are C++; everything else is C.
# Override per-source language flags so cmake doesn't pass -std=c23 to the
# C++ compiler.
set_source_files_properties(
  ${FW_ROOT}/libs/third_party/tinyxml2/tinyxml2.cpp ${RA8_EPUB_CPP_SOURCES}
  ${RA8_RABOOK_COMPILE_CPP_SOURCES} ${RA8_REFLOW_CPP_SOURCES} PROPERTIES LANGUAGE CXX
)
# -w silences the vendored decoders' warnings; -fno-strict-aliasing matches the
# app build (cmake/ra8_add_app.cmake) -- these decoders type-pun through byte
# buffers (arm-gcc miscompiles miniz's inflate under strict aliasing at -Og/-O2).
# The host-test build previously avoided the same UB only by incidentally running
# at -O0; set it structurally here too so a new vendored decoder, or a host build
# raised to -Og, cannot resurface it (T5-16).
set_source_files_properties(
  ${RA8_EPUB_THIRD_PARTY} PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing"
)

# Vendored xz-embedded decoder (SOUP): silence its warnings and disable strict
# aliasing like the other vendored decoders. Its allocator and mode selection
# come from the first-party porting header libs/ra8_unarch/inc/xz_config.h
# (zero-heap pool, XZ_PREALLOC only); the first-party ra8_unarch wrapper
# sources are held to the full project warning bar and are NOT listed here.
set_source_files_properties(
  ${RA8_XZ_THIRD_PARTY} PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing"
)

# Vendored libwebp decoder (SOUP): warning silence, -fno-strict-aliasing (it
# type-puns through byte buffers) and -DRA8_WEBP_USE_ARENA, which is what routes
# WebPSafe{Malloc,Calloc,Free} through the heap-free ra8_webp bump arena. The
# flag set is defined once in cmake/ra8_webp_vendor.cmake so this build, the
# firmware apps and the host tools cannot disagree about it -- the first-party
# ra8_webp facade/arena sources stay on the full project warning bar and are not
# passed here.
ra8_webp_apply_soup_flags(${RA8_WEBP_THIRD_PARTY})

# Route stb_truetype's per-glyph scratch (vertices, rasteriser edge/point
# lists) through the no-heap arena in libs/ra8_reflow/src/ra8_stbtt_alloc.c
# instead of libc malloc, so the host test build exercises the exact
# heap-free path the firmware uses. The -include makes the allocator
# declarations visible inside the vendored single-TU implementation
# without modifying SOUP. (Appends to the -w set above.)
set_property(
  SOURCE ${FW_ROOT}/libs/third_party/stb/stb_truetype_impl.c
  APPEND
  PROPERTY COMPILE_OPTIONS
           -include
           "${FW_ROOT}/libs/ra8_reflow/inc/ra8_stbtt_alloc.h"
           "-DSTBTT_malloc(x,u)=ra8_stbtt_malloc(x)"
           "-DSTBTT_free(x,u)=ra8_stbtt_free(x)"
)

# Strip MC/DC / coverage instrumentation from vendored third_party
# sources. Under DO-178C these are SOUP (Software Of Unknown
# Pedigree) and are exempted from MC/DC measurement -- see
# docs/MCDC.md. Per-file COMPILE_OPTIONS overrides the directory
# add_compile_options() set above when RA8_MCDC=ON.
if(RA8_MCDC)
  set(RA8_MCDC_THIRD_PARTY_SOURCES ${RA8_EPUB_THIRD_PARTY} ${RA8_XZ_THIRD_PARTY}
                                   ${RA8_WEBP_THIRD_PARTY}
  )
  foreach(_src ${RA8_MCDC_THIRD_PARTY_SOURCES})
    set_property(
      SOURCE ${_src}
      APPEND
      PROPERTY COMPILE_OPTIONS
               -fno-coverage-mcdc
               -fno-coverage-mapping
               -fno-profile-instr-generate
               -w
    )
  endforeach()
endif()
if(RA8_REFLOW_USE_LITEHTML)
  set_source_files_properties(
    ${FW_ROOT}/libs/ra8_reflow/v2/ra8_reflow_v2.cpp PROPERTIES COMPILE_OPTIONS -w
  )
  target_include_directories(ra8_core_hal PUBLIC ${FW_ROOT}/libs/third_party/litehtml/include)
  target_link_libraries(ra8_core_hal PUBLIC litehtml gumbo)
endif()
target_compile_features(ra8_core_hal PUBLIC cxx_std_17)

# C++ enabled here only (the top-level project is C/ASM-only since the
# cross-compile path doesn't ship a C++ runtime). tinyxml2 + ra8_epub_xml_shim
# in libs/ra8_epub need C++17 for tinyxml2.
enable_language(CXX)

# ---------------------------------------------------------------------------
# ra8_core_hal_fuzz: slim mirror of ra8_core_hal for the libFuzzer harnesses
# (RA8_FUZZ=ON only; see tests/fuzz/CMakeLists.txt).
#
# ra8_core_hal bundles tests/mocks/ra8_sim_mmap.c, whose load-time MAP_FIXED
# constructor maps a peripheral backing window at 0xE0000000. Under
# AddressSanitizer (the Linux fuzz config) that address is inside ASan's
# reserved shadow gap, so the constructor aborts every harness before
# main() (issue #193). This object library is exactly ra8_core_hal MINUS
# ra8_sim_mmap.c: the pure-computation harnesses link it and never run that
# constructor. The two harnesses that genuinely poke peripheral registers
# (fuzz_ra8_canfd, fuzz_ra8_usb_pal) add ra8_sim_mmap.c back on their own link
# line, where it is compiled under ASan and skips the shadow-gap region
# (see ra8_sim_region_mappable() in tests/mocks/ra8_sim_mmap.c). ra8_core_hal
# itself is untouched, so the host unit-test build keeps the full mapping.
#
# Sources and per-source properties (LANGUAGE, -w, MC/DC opt-out, the
# stb_truetype -include) are shared with ra8_core_hal because
# set_source_files_properties() is directory-scoped: both libraries pick
# them up from the same source paths. Only built when RA8_FUZZ=ON, so the
# ordinary `make test` build never compiles this second object set.
# ---------------------------------------------------------------------------
option(RA8_FUZZ "Build libFuzzer harnesses for parsers (clang only)" OFF)
if(RA8_FUZZ)
  get_target_property(_ra8_core_hal_srcs ra8_core_hal SOURCES)
  list(REMOVE_ITEM _ra8_core_hal_srcs ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra8_sim_mmap.c)
  add_library(ra8_core_hal_fuzz OBJECT ${_ra8_core_hal_srcs})
  ra8_target_enable_project_warnings(ra8_core_hal_fuzz STACK_USAGE_BYTES 0)
  target_compile_options(
    ra8_core_hal_fuzz PRIVATE -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable
                              -Wno-address-of-packed-member
  )
  target_include_directories(
    ra8_core_hal_fuzz PUBLIC $<TARGET_PROPERTY:ra8_core_hal,INTERFACE_INCLUDE_DIRECTORIES>
  )
  target_compile_features(ra8_core_hal_fuzz PUBLIC cxx_std_17)
  if(RA8_REFLOW_USE_LITEHTML)
    target_link_libraries(ra8_core_hal_fuzz PUBLIC litehtml gumbo)
  endif()
endif()

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
function(ra8_add_test name)
  add_executable(${name} ${name}.c $<TARGET_OBJECTS:ra8_core_hal>)
  target_compile_options(
    ${name}
    PRIVATE -Wall
            -Wextra
            -Werror
            -Wno-unused-function
            -Wno-unused-parameter
            -Wno-unused-variable
            -Wno-address-of-packed-member
  )
  set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)
  target_include_directories(
    ${name}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${FW_ROOT}/libs/ra8_core/inc
            ${FW_ROOT}/libs/ra8_hal/inc
            ${FW_ROOT}/libs/ra8_net_pal/inc
            ${FW_ROOT}/libs/ra8_modem_at/inc
            ${FW_ROOT}/libs/ra8_ble_host/inc
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
            ${FW_ROOT}/libs/ra8_jof/inc
            ${FW_ROOT}/libs/ra8_longstrip/inc
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
            ${FW_ROOT}/libs/third_party/xz_embedded
            ${FW_ROOT}/src/secure_app
            ${FW_ROOT}/src/secure_app/inc
            # Per-module src/ for MC/DC test access (see CLAUDE.md).
            ${FW_ROOT}/libs/ra8_core/src
            ${FW_ROOT}/libs/ra8_hal/src
            ${FW_ROOT}/libs/ra8_net_pal/src
            ${FW_ROOT}/libs/ra8_modem_at/src
            ${FW_ROOT}/libs/ra8_ble_host/src
            ${FW_ROOT}/libs/ra8_tls/src
            ${FW_ROOT}/libs/ra8_usb_pal/src
            ${FW_ROOT}/libs/ra8_fs/src
            ${FW_ROOT}/libs/ra8_io/src
            ${FW_ROOT}/libs/ra8_ftl/src
            ${FW_ROOT}/libs/ra8_sdmmc_spi/src
            ${FW_ROOT}/libs/ra8_gfx/src
            ${FW_ROOT}/libs/ra8_nsc/src
            ${FW_ROOT}/libs/ra8_ota/src
            ${FW_ROOT}/libs/ra8_dfu/src
            ${FW_ROOT}/libs/ra8_display_pal/src
            ${FW_ROOT}/libs/ra8_power_profile/src
            ${FW_ROOT}/libs/ra8_touch_cal/src
            ${FW_ROOT}/libs/ra8_epd_cal/src
            ${FW_ROOT}/libs/ra8_mpu/src
            ${FW_ROOT}/libs/ra8_psa_crypto/src
            ${FW_ROOT}/libs/ra8_wdt_supervisor/src
            ${FW_ROOT}/libs/ra8_board_ek_ra8d2/src
            ${FW_ROOT}/libs/ra8_lsm6dso/src
            ${FW_ROOT}/libs/ra8_epub/src
            ${FW_ROOT}/libs/ra8_comic/src
            ${FW_ROOT}/libs/ra8_unarch/src
            ${FW_ROOT}/libs/ra8_jof/src
            ${FW_ROOT}/libs/ra8_longstrip/src
            ${FW_ROOT}/libs/ra8_reflow/src
            ${FW_ROOT}/libs/ra8_tz_secure_boot/src
            ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  )
  #  if(APPLE)
  #    target_link_options(${name} PRIVATE
  #      "-Wl,-pagezero_size,0x4000"
  #      "-Wl,-segalign,0x4000"
  #    )
  #  endif()
  if(RA8_REFLOW_USE_LITEHTML)
    # ra8_core_hal is an OBJECT library, so link deps don't propagate
    # automatically; bind litehtml/gumbo directly into each test exe
    # so the v2 adapter's symbols resolve.
    target_link_libraries(${name} PRIVATE litehtml gumbo)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# Auto-discover every test_*.c file in this directory and register it
# via ra8_add_test(). Dropping a new test_foo.c file is enough -- no
# manual list edit required. (CONFIGURE_DEPENDS means CMake re-globs
# on the next build if the directory contents changed.)
file(GLOB RA8_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)

# v1's test_ra8_reflow.c and its split sibling test_ra8_reflow_api_mcdc.c
# assert on glyph-array internals that v2 deliberately does not populate
# (shared fixture: tests/support/reflow_v1_test_util.h); drop them under
# v2 and add the C++ v2 test below in their place.
if(RA8_REFLOW_USE_LITEHTML)
  list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_reflow.c)
  list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_reflow_api_mcdc.c)
endif()

# test_coverage_compile_all.c is the coverage aggregator: a single
# no-op TU whose only purpose is to force every first-party source
# to be linked with the active coverage instrumentation flags so
# llvm-cov / gcovr can compute uniform repository-wide coverage.
# It contributes zero functional test value, so we exclude it from
# the fast `make test` flow (RA8_MCDC=OFF) and only build it when
# `RA8_MCDC=ON`.
if(NOT RA8_MCDC)
  list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_coverage_compile_all.c)
endif()

# test_psa_real_kat.c links the REAL crypto backend (TF-PSA-Crypto), not the
# sim'd ra8_core_hal, so it is registered by hand below rather than through the
# ra8_add_test() auto-glob (which would splice in the RA8_SIMULATOR_MODE build).
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_psa_real_kat.c)

# test_ra8_cache_store.c needs the vendored LevelX NOR sources + a RAM NOR
# simulator + LX_STANDALONE_ENABLE, so it is registered by hand below rather
# than through the ra8_add_test() auto-glob.
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_cache_store.c)

# test_cache_store_demo.c (issue #257) compiles the ra8_cache_store_demo example
# core + RAM NOR driver from examples/ plus the vendored LevelX NOR sources with
# LX_STANDALONE_ENABLE, so it is registered by hand below rather than through the
# ra8_add_test() auto-glob.
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_cache_store_demo.c)

# test_ra8_rsip_devsec_failclosed.c (issue #216) must compile ra8_rsip_devsec.c
# with the stub-crypto guard flags UNDEFINED so the production fail-closed #else
# is the body under test. The rest of the host build force-defines
# RA8_SIMULATOR_MODE, so it is registered by hand below rather than through the
# ra8_add_test() auto-glob (which would splice in the RA8_SIMULATOR_MODE build).
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_rsip_devsec_failclosed.c)

# test_ra8_npu.c (issue #221) drives the Arm Ethos-U55 NPU driver, whose body in
# ra8_npu.c is device-gated behind RA8_HAS_NPU (RA8P1-only). The shared ra8_core_hal
# object library is compiled for the default RA8D2, so its ra8_npu.c is an EMPTY
# TU with no NPU symbols. It is registered by hand below with -DRA8_DEVICE_RA8P1
# (which makes ra8_device.h define RA8_HAS_NPU) so a live ra8_npu.c is compiled and
# linked; the auto-glob would build it for the RA8D2 and fail to resolve the NPU
# API. This is the same "compile one TU under a different device profile" pattern
# as test_ra8_rsip_devsec_failclosed above.
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_npu.c)

# test_ra8_ethosu_shim.c (issue #228) drives the Arm ethos-u-core-driver -> ra8_npu
# adapter (ra8_ethosu_shim.c), whose body -- like ra8_npu.c -- is device-gated behind
# RA8_HAS_NPU (RA8P1-only). The shared ra8_core_hal object library is compiled for the
# default RA8D2, so its ra8_ethosu_shim.c / ra8_npu.c are EMPTY TUs. It is registered by
# hand below with -DRA8_DEVICE_RA8P1 (same pattern as test_ra8_npu) so live adapter +
# driver bodies are compiled and linked; the auto-glob would build it for the RA8D2 and
# fail to resolve the ethosu_* / ra8_npu_* API.
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_ethosu_shim.c)

# test_ra8_npu_loader.c (issue #227) drives the .npub Vela-blob loader
# (ra8_npu_loader.c), which -- like ra8_npu.c -- is device-gated behind
# RA8_HAS_NPU (RA8P1-only) and turns a committed golden model container
# (tools/vela/generated/ra8_npu_model_addk_sim.h) into an ra8_npu_job_t. It is
# registered by hand below with -DRA8_DEVICE_RA8P1 (same pattern as test_ra8_npu)
# so the live loader + driver bodies compile and link; the auto-glob would build
# it for the RA8D2 and fail to resolve the ra8_npu_* API.
list(REMOVE_ITEM RA8_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_npu_loader.c)

foreach(src ${RA8_TEST_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  ra8_add_test(${name})
endforeach()

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

# ---------------------------------------------------------------------------
# test_stub_crypto_gate (issue #180): reachability half of the stub-vs-production
# crypto gate. The auto-glob above already registered it (it links ra8_core_hal,
# built under RA8_SIMULATOR_MODE, so the guarded #if branch is active). Define
# RA8_INSECURE_STUB_CRYPTO on it too, so it faithfully compiles under the opt-in
# flag the gate governs -- both flags select the same insecure #if branch, and
# the test asserts those stub bodies are reachable-but-insecure (a fail-closed
# #else would return k_ra8_err_not_supported and fail every case).
# ---------------------------------------------------------------------------
if(TARGET test_stub_crypto_gate)
  target_compile_definitions(test_stub_crypto_gate PRIVATE RA8_INSECURE_STUB_CRYPTO)
endif()

# ---------------------------------------------------------------------------
# test_ra8_rsip_devsec_failclosed (issue #216): production fail-closed reachability
# for the RSIP device-security path (lifecycle / debug / tamper / DPA arm). The
# rest of the host build compiles ra8_rsip_devsec.c under RA8_SIMULATOR_MODE (the
# guarded #if branch, exercised by test_life / test_debug_level / test_tamper in
# test_ra8_rsip_devsec.c). This self-contained target (no ra8_core_hal, no sim) rebuilds
# JUST that TU with the two guard flags UNDEFINED so the production #else is the
# compiled body, and asserts every entry point returns k_ra8_err_not_supported and
# writes no fabricated state. -U wins over the directory-level -D because CMake
# emits DEFINES before FLAGS on the compile line. -DRA8_LOG_LEVEL=0 makes the
# RA8_CHECK_NULL_PTR log a no-op so the TU needs no ra8_log backend to link.
# ---------------------------------------------------------------------------
add_executable(
  test_ra8_rsip_devsec_failclosed ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_rsip_devsec_failclosed.c
                                  ${FW_ROOT}/libs/ra8_hal/src/ra8_rsip_devsec.c
)
target_include_directories(
  test_ra8_rsip_devsec_failclosed PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FW_ROOT}/libs/ra8_core/inc
                                          ${FW_ROOT}/libs/ra8_hal/inc
)
target_compile_options(
  test_ra8_rsip_devsec_failclosed
  PRIVATE -Wall
          -Wextra
          -Werror
          -Wno-unused-function
          -Wno-unused-parameter
          -Wno-unused-variable
          -URA8_SIMULATOR_MODE
          -URA8_INSECURE_STUB_CRYPTO
          -DRA8_LOG_LEVEL=0
)
add_test(NAME test_ra8_rsip_devsec_failclosed COMMAND test_ra8_rsip_devsec_failclosed)

# ---------------------------------------------------------------------------
# test_ra8_npu (issue #221): Arm Ethos-U55 NPU command/queue driver foundation.
# ra8_npu.c is device-gated behind RA8_HAS_NPU, so it is recompiled here with
# -DRA8_DEVICE_RA8P1 (ra8_device.h then defines RA8_HAS_NPU) to make the driver body
# live. It links ra8_core_hal for ra8_mstp / ra8_log / ra8_err and the host MMIO
# backing store (tests/mocks/ra8_sim_mmap.c) -- the NPU window 0x40140000 lies
# inside the emulated peripheral region, so register writes land in RAM and the
# test asserts the QBASE / QSIZE / BASEPn / CMD submission sequence. Only ra8_npu.c
# and the test TU see RA8_DEVICE_RA8P1; the prebuilt ra8_core_hal stays RA8D2, so
# its own (empty) ra8_npu TU never collides with this one. LINKER_LANGUAGE CXX
# because ra8_core_hal bundles C++ TUs (tinyxml2 etc.).
# ---------------------------------------------------------------------------
add_executable(
  test_ra8_npu ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_npu.c ${FW_ROOT}/libs/ra8_hal/src/ra8_npu.c
               $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_npu PROPERTIES LINKER_LANGUAGE CXX)
target_compile_definitions(test_ra8_npu PRIVATE RA8_DEVICE_RA8P1)
target_compile_options(
  test_ra8_npu
  PRIVATE -Wall
          -Wextra
          -Werror
          -Wno-unused-function
          -Wno-unused-parameter
          -Wno-unused-variable
)
target_include_directories(
  test_ra8_npu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FW_ROOT}/libs/ra8_core/inc
                       ${FW_ROOT}/libs/ra8_hal/inc ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
if(RA8_REFLOW_USE_LITEHTML)
  target_link_libraries(test_ra8_npu PRIVATE litehtml gumbo)
endif()
add_test(NAME test_ra8_npu COMMAND test_ra8_npu)

# ---------------------------------------------------------------------------
# test_ra8_ethosu_shim (issue #228): Arm ethos-u-core-driver C API adapter over
# ra8_npu. Both ra8_ethosu_shim.c and ra8_npu.c are device-gated behind RA8_HAS_NPU,
# so both are recompiled here with -DRA8_DEVICE_RA8P1 (ra8_device.h then defines
# RA8_HAS_NPU) to make the adapter + driver bodies live. It links ra8_core_hal for
# ra8_mstp / ra8_log / ra8_err and the host MMIO backing store (tests/mocks/
# ra8_sim_mmap.c) -- the NPU window 0x40140000 lies inside the emulated peripheral
# region, so the adapter's register writes (via ra8_npu) land in RAM and the test
# asserts the QBASE / QSIZE / BASEPn / CMD programming plus the int return code.
# Only these two TUs and the test see RA8_DEVICE_RA8P1; the prebuilt ra8_core_hal
# stays RA8D2, so its own (empty) ra8_npu / ra8_ethosu_shim TUs never collide with
# these. LINKER_LANGUAGE CXX because ra8_core_hal bundles C++ TUs (tinyxml2 etc.).
# ---------------------------------------------------------------------------
add_executable(
  test_ra8_ethosu_shim
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_ethosu_shim.c ${FW_ROOT}/libs/ra8_hal/src/ra8_npu.c
  ${FW_ROOT}/libs/ra8_hal/src/ra8_ethosu_shim.c $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_ethosu_shim PROPERTIES LINKER_LANGUAGE CXX)
target_compile_definitions(test_ra8_ethosu_shim PRIVATE RA8_DEVICE_RA8P1)
target_compile_options(
  test_ra8_ethosu_shim
  PRIVATE -Wall
          -Wextra
          -Werror
          -Wno-unused-function
          -Wno-unused-parameter
          -Wno-unused-variable
)
target_include_directories(
  test_ra8_ethosu_shim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FW_ROOT}/libs/ra8_core/inc
                               ${FW_ROOT}/libs/ra8_hal/inc ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
if(RA8_REFLOW_USE_LITEHTML)
  target_link_libraries(test_ra8_ethosu_shim PRIVATE litehtml gumbo)
endif()
add_test(NAME test_ra8_ethosu_shim COMMAND test_ra8_ethosu_shim)

# ---------------------------------------------------------------------------
# test_ra8_npu_loader (issue #227): the .npub Vela-blob loader. ra8_npu_loader.c
# and ra8_npu.c are device-gated behind RA8_HAS_NPU, so both are recompiled here
# with -DRA8_DEVICE_RA8P1 (ra8_device.h then defines RA8_HAS_NPU) to make the
# loader + driver bodies live. It includes the committed golden model header
# generated by tools/vela/vela_gen.py (tools/vela/generated/), links ra8_core_hal
# for ra8_mstp / ra8_log / ra8_err and the host MMIO backing store, byte-pins the
# extracted command stream + region layout, and runs the job through a host
# mirror of the board_sim NPU model. Only these two TUs and the test see
# RA8_DEVICE_RA8P1; the prebuilt ra8_core_hal stays RA8D2, so its own (empty)
# ra8_npu / ra8_npu_loader TUs never collide with these. LINKER_LANGUAGE CXX
# because ra8_core_hal bundles C++ TUs (tinyxml2 etc.).
# ---------------------------------------------------------------------------
add_executable(
  test_ra8_npu_loader
  ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_npu_loader.c ${FW_ROOT}/libs/ra8_hal/src/ra8_npu.c
  ${FW_ROOT}/libs/ra8_hal/src/ra8_npu_loader.c $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_npu_loader PROPERTIES LINKER_LANGUAGE CXX)
target_compile_definitions(test_ra8_npu_loader PRIVATE RA8_DEVICE_RA8P1)
target_compile_options(
  test_ra8_npu_loader
  PRIVATE -Wall
          -Wextra
          -Werror
          -Wno-unused-function
          -Wno-unused-parameter
          -Wno-unused-variable
)
target_include_directories(
  test_ra8_npu_loader
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${FW_ROOT}/libs/ra8_core/inc
          ${FW_ROOT}/libs/ra8_hal/inc
          ${FW_ROOT}/tools/vela/generated
          ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
if(RA8_REFLOW_USE_LITEHTML)
  target_link_libraries(test_ra8_npu_loader PRIVATE litehtml gumbo)
endif()
add_test(NAME test_ra8_npu_loader COMMAND test_ra8_npu_loader)

# ---------------------------------------------------------------------------
# T5-04: real-backend crypto known-answer test. The rest of the host build
# compiles ra8_psa_crypto.c under RA8_SIMULATOR_MODE (a deterministic AES-GCM
# stand-in + an HMAC-tautology ECDSA "verify"), so the AEAD / signature paths
# are never checked against a real cipher. This target links the vendored
# TF-PSA-Crypto library directly -- the exact psa_* primitives the production
# !RA8_SIMULATOR_MODE path calls -- and pins them to NIST/RFC vectors. It is a
# self-contained executable (no ra8_core_hal, no sim). TF-PSA-Crypto is SOUP:
# compile it -w -fno-strict-aliasing like the other vendored trees; it is
# already excluded from the coverage filter (libs/third_party/).
set(_RA8_TFPSA_KAT_DIR ${FW_ROOT}/libs/third_party/tf-psa-crypto)
file(
  GLOB
  _RA8_TFPSA_KAT_SRC
  CONFIGURE_DEPENDS
  ${_RA8_TFPSA_KAT_DIR}/core/*.c
  ${_RA8_TFPSA_KAT_DIR}/drivers/builtin/src/*.c
  ${_RA8_TFPSA_KAT_DIR}/platform/*.c
  ${_RA8_TFPSA_KAT_DIR}/utilities/*.c
  ${_RA8_TFPSA_KAT_DIR}/extras/*.c
)
set(_RA8_TFPSA_KAT_INC
    ${_RA8_TFPSA_KAT_DIR}/include
    ${_RA8_TFPSA_KAT_DIR}/drivers/builtin/include
    ${_RA8_TFPSA_KAT_DIR}/core
    ${_RA8_TFPSA_KAT_DIR}/dispatch
    ${_RA8_TFPSA_KAT_DIR}/drivers/builtin/src
    ${_RA8_TFPSA_KAT_DIR}/platform
    ${_RA8_TFPSA_KAT_DIR}/utilities
    ${_RA8_TFPSA_KAT_DIR}/extras
)
add_library(ra8_tfpsa_host_kat OBJECT ${_RA8_TFPSA_KAT_SRC})
target_include_directories(ra8_tfpsa_host_kat PRIVATE ${_RA8_TFPSA_KAT_INC})
target_compile_options(ra8_tfpsa_host_kat PRIVATE -w -fno-strict-aliasing)
add_executable(
  test_psa_real_kat ${CMAKE_CURRENT_SOURCE_DIR}/test_psa_real_kat.c
                    $<TARGET_OBJECTS:ra8_tfpsa_host_kat>
)
target_include_directories(test_psa_real_kat PRIVATE ${_RA8_TFPSA_KAT_INC})
target_compile_options(test_psa_real_kat PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_psa_real_kat COMMAND test_psa_real_kat)

if(RA8_REFLOW_USE_LITEHTML)
  add_executable(
    test_ra8_reflow_v2 ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_reflow_v2.cpp
                       $<TARGET_OBJECTS:ra8_core_hal>
  )
  set_target_properties(test_ra8_reflow_v2 PROPERTIES LINKER_LANGUAGE CXX)
  target_compile_options(
    test_ra8_reflow_v2
    PRIVATE -Wall
            -Wextra
            -Wno-unused-function
            -Wno-unused-parameter
            -Wno-unused-variable
            -Wno-address-of-packed-member
  )
  target_compile_features(test_ra8_reflow_v2 PRIVATE cxx_std_17)
  target_include_directories(
    test_ra8_reflow_v2
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${FW_ROOT}/libs/ra8_core/inc
            ${FW_ROOT}/libs/ra8_reflow/inc
            ${FW_ROOT}/libs/third_party/litehtml/include
            ${FW_ROOT}/libs/third_party/stb
            ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  )
  target_link_libraries(test_ra8_reflow_v2 PRIVATE litehtml gumbo)
  #  if(APPLE)
  #    target_link_options(test_ra8_reflow_v2 PRIVATE
  #      "-Wl,-pagezero_size,0x4000"
  #      "-Wl,-segalign,0x4000"
  #    )
  #  endif()
  add_test(NAME test_ra8_reflow_v2 COMMAND test_ra8_reflow_v2)
endif()

# ---------------------------------------------------------------------------
# C++ test target for ra8_epub_xml_shim. The XML shim is a C++ TU that is
# compiled into the ra8_core_hal OBJECT library above; this executable
# drives its public C-linkage entry points to land MC/DC vectors on the
# decisions documented in tests/test_ra8_epub_xml_shim.cpp.
# ---------------------------------------------------------------------------
add_executable(
  test_ra8_epub_xml_shim ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_epub_xml_shim.cpp
                         $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_epub_xml_shim PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(
  test_ra8_epub_xml_shim
  PRIVATE -Wall
          -Wextra
          -Wno-unused-function
          -Wno-unused-parameter
          -Wno-unused-variable
          -Wno-address-of-packed-member
)
target_compile_features(test_ra8_epub_xml_shim PRIVATE cxx_std_17)
target_include_directories(
  test_ra8_epub_xml_shim
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${FW_ROOT}/libs/ra8_core/inc
          ${FW_ROOT}/libs/ra8_epub/inc
          # ra8_epub_xml_shim_internal.h declares the shim's four extern "C" entry
          # points and their result struct; this test consumes that contract instead
          # of restating it, so the library's src/ has to be reachable.
          ${FW_ROOT}/libs/ra8_epub/src
          ${FW_ROOT}/libs/third_party/tinyxml2
          ${FW_ROOT}/libs/third_party/stb
          ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
if(RA8_REFLOW_USE_LITEHTML)
  target_link_libraries(test_ra8_epub_xml_shim PRIVATE litehtml gumbo)
endif()
#if(APPLE)
#  target_link_options(test_ra8_epub_xml_shim PRIVATE
#    "-Wl,-pagezero_size,0x4000"
#    "-Wl,-segalign,0x4000"
#  )
#endif()
add_test(NAME test_ra8_epub_xml_shim COMMAND test_ra8_epub_xml_shim)

# ---------------------------------------------------------------------------
# White-box line-coverage target for ra8_epub_xml_shim. This test #includes the
# shim source with its four exported symbols renamed (*_cov) so the anonymous-
# namespace helpers can be driven directly with the edge inputs the public API
# filters out. It adds libs/ra8_epub/src to the include path so the
# `#include "ra8_epub_xml_shim.cpp"` resolves; tinyxml2 links from ra8_core_hal.
# ---------------------------------------------------------------------------
add_executable(
  test_ra8_epub_xml_shim_cov ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_epub_xml_shim_cov.cpp
                             $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_epub_xml_shim_cov PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(
  test_ra8_epub_xml_shim_cov
  PRIVATE -Wall
          -Wextra
          -Wno-unused-function
          -Wno-unused-parameter
          -Wno-unused-variable
          -Wno-address-of-packed-member
)
target_compile_features(test_ra8_epub_xml_shim_cov PRIVATE cxx_std_17)
target_include_directories(
  test_ra8_epub_xml_shim_cov
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${FW_ROOT}/libs/ra8_core/inc
          ${FW_ROOT}/libs/ra8_epub/inc
          ${FW_ROOT}/libs/ra8_epub/src
          ${FW_ROOT}/libs/third_party/tinyxml2
          ${FW_ROOT}/libs/third_party/stb
          ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
if(RA8_REFLOW_USE_LITEHTML)
  target_link_libraries(test_ra8_epub_xml_shim_cov PRIVATE litehtml gumbo)
endif()
add_test(NAME test_ra8_epub_xml_shim_cov COMMAND test_ra8_epub_xml_shim_cov)

# ---------------------------------------------------------------------------
# Complementary line-coverage target for ra8_epub_xml_shim. Unlike the _cov
# sibling, this test is BLACK-BOX: it links the real (un-renamed) production
# entry points from ra8_core_hal and drives their length / parse / root /
# rootfile-resolution / spine-skip / cap / cover-meta / deep-recurse / nav-skip
# legs with fixtures. Calling the un-renamed functions lands the entry-point
# body lines in the shared production object so gcovr merges them into the
# aggregate (a renamed white-box copy would attribute them to a distinct
# function and drop the union). No source #include; tinyxml2 links transitively.
# ---------------------------------------------------------------------------
add_executable(
  test_ra8_epub_xml_shim_cov2 ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_epub_xml_shim_cov2.cpp
                              $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_epub_xml_shim_cov2 PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(
  test_ra8_epub_xml_shim_cov2
  PRIVATE -Wall
          -Wextra
          -Wno-unused-function
          -Wno-unused-parameter
          -Wno-unused-variable
          -Wno-address-of-packed-member
)
target_compile_features(test_ra8_epub_xml_shim_cov2 PRIVATE cxx_std_17)
target_include_directories(
  test_ra8_epub_xml_shim_cov2
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${FW_ROOT}/libs/ra8_core/inc
          ${FW_ROOT}/libs/ra8_epub/inc
          # See test_ra8_epub_xml_shim: the shim's contract lives in
          # ra8_epub_xml_shim_internal.h under the library's src/.
          ${FW_ROOT}/libs/ra8_epub/src
          ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
if(RA8_REFLOW_USE_LITEHTML)
  target_link_libraries(test_ra8_epub_xml_shim_cov2 PRIVATE litehtml gumbo)
endif()
add_test(NAME test_ra8_epub_xml_shim_cov2 COMMAND test_ra8_epub_xml_shim_cov2)

# ---------------------------------------------------------------------------
# C++ test target for ra8_rabook_xml_shim. Drives the public C-linkage entry
# point ra8_rabook_xml_parse_chapter() to verify DOM pre-order and MC/DC
# vectors for the iterative DFS (#149 stage-a XHTML parser).
# ---------------------------------------------------------------------------
add_executable(
  test_ra8_rabook_xml_shim ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_rabook_xml_shim.cpp
                           $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_rabook_xml_shim PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(
  test_ra8_rabook_xml_shim
  PRIVATE -Wall
          -Wextra
          -Wno-unused-function
          -Wno-unused-parameter
          -Wno-unused-variable
          -Wno-address-of-packed-member
)
target_compile_features(test_ra8_rabook_xml_shim PRIVATE cxx_std_17)
target_include_directories(
  test_ra8_rabook_xml_shim
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${FW_ROOT}/libs/ra8_core/inc
          ${FW_ROOT}/libs/ra8_book/inc
          ${FW_ROOT}/libs/ra8_rabook_compile/inc
          ${FW_ROOT}/libs/third_party/tinyxml2
          ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
if(RA8_REFLOW_USE_LITEHTML)
  target_link_libraries(test_ra8_rabook_xml_shim PRIVATE litehtml gumbo)
endif()
add_test(NAME test_ra8_rabook_xml_shim COMMAND test_ra8_rabook_xml_shim)

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
# (lx_nor_ram.c) the ARM example runs, so the host test and the board_sim gate
# exercise byte-identical logic. Reuses the cache_store + LevelX-standalone
# sources globbed for test_ra8_cache_store above; ra8_core_hal supplies ra8_log /
# ra8_err / ra8_check. The example sources live under examples/ (outside the
# coverage filter), so they add no per-file coverage-floor obligation.
# ---------------------------------------------------------------------------
set(RCS_DEMO_DIR ${FW_ROOT}/examples/ek_ra8d2/hw_validated/hil/ra8_cache_store_demo)
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
          # threadx_ble_* -> ra8_ble_host, ...) still resolves its headers instead of a
          # clang-diagnostic-error. The extra dirs are harmless to the actual test build.
          ${FW_ROOT}/libs/ra8_core/inc
          ${FW_ROOT}/libs/ra8_hal/inc
          ${FW_ROOT}/libs/ra8_net_pal/inc
          ${FW_ROOT}/libs/ra8_modem_at/inc
          ${FW_ROOT}/libs/ra8_ble_host/inc
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

# ---------------------------------------------------------------------------
# Optional libFuzzer harnesses (opt-in via -DRA8_FUZZ=ON; clang-only).
# Each fuzz target is built as a separate executable that links against
# the existing ra8_core_hal OBJECT library and adds
# -fsanitize=fuzzer,address,undefined only on its own translation unit.
# Default OFF so the normal host test build is unaffected.
# See docs/FUZZING.md. The RA8_FUZZ option and the slim ra8_core_hal_fuzz
# object library it gates are declared earlier, next to ra8_core_hal.
# ---------------------------------------------------------------------------
add_subdirectory(fuzz)

# ---------------------------------------------------------------------------
# Optional host-side performance benchmarks (opt-in via -DRA8_BENCH=ON).
# Each bench target prints CSV results to stdout. Decoupled from the
# fast unit-test build because the bench binaries build at -O2 and we
# do not want them entangled with coverage/MC-DC instrumentation.
# See docs/PERFORMANCE.md.
# ---------------------------------------------------------------------------
option(RA8_BENCH "Build host-side performance microbenchmarks" OFF)
add_subdirectory(bench)

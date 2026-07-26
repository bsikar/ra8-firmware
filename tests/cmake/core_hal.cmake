# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# ra8_core_hal: the object library every host test links, its per-source
# property overrides, and ra8_core_hal_fuzz -- the slim mirror the libFuzzer
# harnesses use.
#
# The mirror lives beside the original deliberately: it is defined as
# "ra8_core_hal MINUS ra8_sim_mmap.c" by reading the real target's SOURCES,
# so the two cannot drift apart.
#
# Included from tests/CMakeLists.txt. CMake include() is textual within the
# same directory scope, so every variable and target defined here is visible
# to the driver and to the fragments included after it.

add_library(
  ra8_core_hal OBJECT
  ${RA8_CORE_SOURCES}
  ${RA8_HAL_SOURCES}
  ${RA8_NET_PAL_SOURCES}
  ${RA8_MODEM_AT_SOURCES}
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
  ${RA8_DEVCFG_SOURCES}
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
         ${FW_ROOT}/libs/ra8_devcfg/inc
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
         ${FW_ROOT}/libs/ra8_devcfg/src
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

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# ra8_core_hal: the object library every host test links, its per-source
# property overrides, and ra8_core_hal_fuzz -- the slim mirror the libFuzzer
# harnesses use.
#
# The mirror lives beside the original deliberately: it is defined as
# "ra8_core_hal MINUS ra8_fake_mmap.c" by reading the real target's SOURCES,
# so the two cannot drift apart.
#
# Included from tests/CMakeLists.txt. CMake include() is textual within the
# same directory scope, so every variable and target defined here is visible
# to the driver and to the fragments included after it.

add_library(
  ra8_core_hal OBJECT
  ${RA8_CORE_SOURCES}
  ${XML_SOURCES}
  ${RA8_HAL_SOURCES}
  ${RA8_JPEG_SOURCES}
  ${RA8_NET_PAL_SOURCES}
  ${RA8_MODEM_AT_SOURCES}
  ${RA8_TLS_SOURCES}
  ${RA8_USB_PAL_SOURCES}
  ${RA8_FS_SOURCES}
  ${RA8_IF_SOURCES}
  ${RA8_IF_RA8_VFS_SOURCES}
  ${RA8_IO_SOURCES}
  ${COMPRESS_SOURCES}
  ${RA8_AUDIO_SOURCES}
  ${RA8_CAMERA_SOURCES}
  ${RA8_CAMERA_IO_SOURCES}
  ${RA8_FTL_SOURCES}
  ${RA8_MEM_SOURCES}
  ${RA8_SDMMC_SPI_SOURCES}
  ${RA8_GFX_SOURCES}
  ${RA8_UI_SOURCES}
  ${RA8_KEYBOARD_SOURCES}
  ${RA8_BOX_SOURCES}
  ${BOOK_SOURCES}
  ${RABOOK_COMPILE_SOURCES}
  ${RABOOK_COMPILE_CPP_SOURCES}
  ${RABOOK_IMPORT_SOURCES}
  ${RA8_BATT_SOURCES}
  ${RA8_WIDGET_SOURCES}
  ${RA8_APP_SOURCES}
  ${RA8_NSC_SOURCES}
  ${RA8_OTA_SOURCES}
  ${RA8_DISPLAY_PAL_SOURCES}
  ${RA8_POWER_PROFILE_SOURCES}
  ${EPUB_C_SOURCES}
  ${EPUB_CPP_SOURCES}
  ${COMIC_SOURCES}
  ${UNARCH_SOURCES}
  ${JOF_SOURCES}
  ${LONGSTRIP_SOURCES}
  ${ZOOM_SOURCES}
  ${REFLOW_C_SOURCES}
  ${REFLOW_CPP_SOURCES}
  ${EPUB_THIRD_PARTY}
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
  ${RA8_OV5640_SOURCES}
  ${RA8_TZ_SECURE_BOOT_SOURCES}
  ${RA8_DFU_SOURCES}
  ${RA8_DEVCFG_SOURCES}
  ${RA8_WIFI_SOURCES}
  # ThreadX SysTick retune (issue #287). Lives under port/threadx (not
  # libs/), so it is not caught by the libs/ globs above -- add it by
  # hand. Its SYST_RVR/CVR writes compile out under RA8_OFF_TARGET,
  # so the host build exercises only the clock-query + reload arithmetic.
  ${FW_ROOT}/port/threadx/src/cortex_m85/tx_systick_retune.c
  # esp-hosted RA8 + ThreadX port (piece 1b). Lives under port/esp-hosted
  # (not libs/), so the libs/ globs above do not catch it -- list it by
  # hand. Every TU here is host-testable: the ThreadX calls go through the
  # recording shim in port/esp-hosted/tests/inc under
  # RA8_OFF_TARGET, and the SPI and GPIO slices take their bus and pin
  # interfaces through injectable seams.
  ${FW_ROOT}/port/esp-hosted/src/ra8_esp_hosted_fmt.c
  ${FW_ROOT}/port/esp-hosted/src/ra8_esp_hosted_gpio.c
  ${FW_ROOT}/port/esp-hosted/src/ra8_esp_hosted_gpio_edge.c
  ${FW_ROOT}/port/esp-hosted/src/ra8_esp_hosted_log.c
  ${FW_ROOT}/port/esp-hosted/src/ra8_esp_hosted_osi.c
  ${FW_ROOT}/port/esp-hosted/src/ra8_esp_hosted_osi_absent.c
  ${FW_ROOT}/port/esp-hosted/src/ra8_esp_hosted_pins.c
  ${FW_ROOT}/port/esp-hosted/src/ra8_esp_hosted_port.c
  ${FW_ROOT}/port/esp-hosted/src/ra8_esp_hosted_rtos.c
  ${FW_ROOT}/port/esp-hosted/src/ra8_esp_hosted_rtos_pool.c
  ${FW_ROOT}/port/esp-hosted/src/ra8_esp_hosted_rtos_sync.c
  ${FW_ROOT}/port/esp-hosted/src/ra8_esp_hosted_spi.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/src/ra8_fake_mmap.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/src/ra8_fake_irq.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/src/ra8_fake_dma.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/src/ra8_fake_time.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/src/ra8_fake_world.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/src/ra8_fake_mmio.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/src/ra8_fake_xspi_flash.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks/src/ra8_host_asm_stub.c
)
# STACK_USAGE_BYTES 0 disables the per-function stack gate for the host
# build (see the line 41 note): the host ABI pushes wider frames than the
# Cortex-M85, so -Wstack-usage here is a false-positive generator. The real
# target stack budget is enforced by the firmware build + stack_usage_check.py.
ra8_target_enable_project_warnings(ra8_core_hal STACK_USAGE_BYTES 0)
target_compile_options(ra8_core_hal PRIVATE)
target_include_directories(
  ra8_core_hal
  PUBLIC ${FW_ROOT}/libs/ra8_core/inc
         ${FW_ROOT}/apps/shared_libs/xml/inc
         ${FW_ROOT}/libs/ra8_hal/inc
         ${FW_ROOT}/libs/ra8_jpeg/inc
         ${FW_ROOT}/libs/ra8_net_pal/inc
         ${FW_ROOT}/libs/ra8_modem_at/inc
         ${FW_ROOT}/libs/ra8_usb_pal/inc
         ${FW_ROOT}/libs/ra8_fs/inc
         ${FW_ROOT}/libs/if/inc
         ${FW_ROOT}/libs/if_ra8_vfs/inc
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
         # libwebp is included as "src/webp/...", so its ROOT is the include dir.
         ${FW_ROOT}/apps/shared_libs/third_party/libwebp
         ${FW_ROOT}/apps/shared_libs/webp/inc
         ${FW_ROOT}/apps/shared_libs/webp/src
         ${FW_ROOT}/libs/ra8_secure_app/inc
         # Per-module src/ directories exposed for MC/DC test access to
         # internal helpers (see CLAUDE.md "Test access to internal symbols").
         # Tests under tests/ MAY include "<module>_internal.h" to drive MC/DC
         # vectors on production source text. Only first-party libs are listed.
         ${FW_ROOT}/libs/ra8_core/src
         ${FW_ROOT}/libs/ra8_hal/src
         ${FW_ROOT}/libs/ra8_secure_app/src
         ${FW_ROOT}/libs/ra8_jpeg/src
         ${FW_ROOT}/libs/ra8_net_pal/src
         ${FW_ROOT}/libs/ra8_modem_at/src
         ${FW_ROOT}/libs/ra8_tls/src
         ${FW_ROOT}/libs/ra8_usb_pal/src
         ${FW_ROOT}/libs/ra8_fs/src
         ${FW_ROOT}/libs/if/src
         ${FW_ROOT}/libs/if_ra8_vfs/src
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
         ${FW_ROOT}/libs/ra8_display_pal/src
         ${FW_ROOT}/libs/ra8_power_profile/src
         ${FW_ROOT}/libs/ra8_touch_cal/src
         ${FW_ROOT}/libs/ra8_epd_cal/src
         ${FW_ROOT}/libs/ra8_mpu/src
         ${FW_ROOT}/libs/ra8_psa_crypto/src
         ${FW_ROOT}/libs/ra8_wdt_supervisor/src
         ${FW_ROOT}/libs/ra8_board_ek_ra8d2/src
         ${FW_ROOT}/libs/ra8_lsm6dso/src
         ${FW_ROOT}/apps/shared_libs/epub/src
         ${FW_ROOT}/apps/shared_libs/comic/src
         ${FW_ROOT}/apps/shared_libs/unarch/src
         ${FW_ROOT}/apps/shared_libs/jof/src
         ${FW_ROOT}/apps/shared_libs/longstrip/src
         ${FW_ROOT}/apps/shared_libs/zoom/src
         ${FW_ROOT}/apps/shared_libs/reflow/src
         ${FW_ROOT}/libs/ra8_tz_secure_boot/src
         ${CMAKE_CURRENT_SOURCE_DIR}/support/inc
         ${CMAKE_CURRENT_SOURCE_DIR}/mocks/inc
)
# Override the remaining C++ sources so CMake does not pass -std=c23 to them.
set_source_files_properties(${REFLOW_CPP_SOURCES} PROPERTIES LANGUAGE CXX)
# -fno-strict-aliasing matches the app build (cmake/ra8_add_app.cmake) -- these
# decoders type-pun through byte buffers (arm-gcc miscompiles miniz's inflate
# under strict aliasing at -Og/-O2). The host-test build previously avoided the
# same UB only by incidentally running at -O0; set it structurally here too so a
# new vendored decoder, or a host build raised to -Og, cannot resurface it
# (T5-16).
#
# The warning list below used to open with a blanket -Wno-error and then repeat
# five of its own entries. -Wno-error is not a diagnostic suppression: it demotes
# every class nobody enumerated, permanently, including classes a future compiler
# adds. Compiling EPUB_THIRD_PARTY (miniz.c, stb_truetype_impl.c,
# stb_image_impl.c) with the enumerated list applied and -Wno-error removed
# showed it was absorbing exactly two classes, -Wduplicated-branches and
# -Wmissing-declarations, both out of stb_image.h; both are enumerated below and
# the blanket is gone. Every remaining name was then re-measured ONE AT A TIME
# (that flag removed, all the others kept) against this listfile's own compile
# database under BOTH pinned host compilers -- gcc 14.2.0 and clang 18 -- at the
# -O0 this build uses. Names that masked nothing under either compiler were
# deleted rather than kept because the code is vendored: -Wno-sign-conversion,
# -Wno-implicit-int-conversion, -Wno-implicit-float-conversion and
# -Wno-implicit-int-float-conversion are all members of clang's -Wconversion
# group and of gcc's, so -Wno-conversion already covered every one of them.
set(_ra8_wno_7
    -Wno-conversion # miniz/stb narrowing; also covers the whole clang
                    # -Wconversion group (sign / implicit-int / implicit-float)
    -Wno-unused-parameter # stb_truetype.h stbtt_GetGlyphBox(info) formals
    -Wno-cast-qual # miniz.c casts away const on its byte buffers
    -Wno-cast-align # clang-18 only: stb_image.h casts uint8_t* to stbi__uint16*
    -Wno-double-promotion # stb_truetype.h float -> double in its math hooks
    -Wno-bad-function-cast # stb_truetype_impl.c casts a float call result to int
    -Wno-missing-prototypes # stb_image.h stbi_hdr_to_ldr_gamma has no prototype
    -Wno-missing-declarations # gcc only: stb_image.h globals with no declaration
    -fno-strict-aliasing
)
# -Wduplicated-branches is a GCC-only spelling and clang REJECTS an unknown
# -Wno-<name> outright under -Werror (-Wunknown-warning-option), so it cannot sit
# in a list this file also hands to the clang-18 lane. Appended by compiler id
# rather than hidden behind a blanket. (-Wno-missing-declarations above needs no
# such guard: clang-18 accepts the spelling and simply never raises it here.)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  list(APPEND _ra8_wno_7 -Wno-duplicated-branches) # stb_image has identical exit arms.
endif()
set_source_files_properties(${EPUB_THIRD_PARTY} PROPERTIES COMPILE_OPTIONS "${_ra8_wno_7}")

# Vendored xz-embedded decoder (SOUP): silence its warnings and disable strict
# aliasing like the other vendored decoders. Its allocator and mode selection
# come from the first-party porting header apps/shared_libs/unarch/inc/xz_config.h
# (zero-heap pool, XZ_PREALLOC only); the first-party unarch wrapper
# sources are held to the full project warning bar and are NOT listed here.
# Measured one flag at a time on all four xz TUs under gcc 14.2.0 and clang 18 at
# -O0: only -Wconversion fires (xz_config.h size_t -> uint32_t in the first-party
# porting header the decoder includes). -Wno-error masked nothing at all,
# -Wno-shorten-64-to-32 never fired on either compiler, and -Wno-sign-conversion
# is subsumed by -Wno-conversion on both; all three are gone.
set(_ra8_test_xz_compile_options
    -Wno-conversion # xz_config.h narrows size_t to the decoder's fixed uint32_t ABI.
    -fno-strict-aliasing
)
set_source_files_properties(
  ${RA8_XZ_THIRD_PARTY} PROPERTIES COMPILE_OPTIONS "${_ra8_test_xz_compile_options}"
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
# lists) through the no-heap arena in apps/shared_libs/reflow/src/ra8_stbtt_alloc.c
# instead of libc malloc, so the host test build exercises the exact
# heap-free path the firmware uses. The -include makes the allocator
# declarations visible inside the vendored single-TU implementation
# without modifying SOUP. (Appends to the -w set above.)
set_property(
  SOURCE ${FW_ROOT}/apps/shared_libs/third_party/stb/stb_truetype_impl.c
  APPEND
  PROPERTY COMPILE_OPTIONS
           -include
           "${FW_ROOT}/apps/shared_libs/reflow/inc/ra8_stbtt_alloc.h"
           "-DSTBTT_malloc(x,u)=ra8_stbtt_malloc(x)"
           "-DSTBTT_free(x,u)=ra8_stbtt_free(x)"
)

# Strip MC/DC / coverage instrumentation from vendored third_party
# sources. Under DO-178C these are SOUP (Software Of Unknown
# Pedigree) and are exempted from MC/DC measurement -- see
# docs/MCDC.md. Per-file COMPILE_OPTIONS overrides the directory
# add_compile_options() set above when RA8_MCDC=ON.
if(RA8_MCDC)
  set(RA8_MCDC_THIRD_PARTY_SOURCES ${EPUB_THIRD_PARTY} ${RA8_XZ_THIRD_PARTY}
                                   ${RA8_WEBP_THIRD_PARTY}
  )
  foreach(_src ${RA8_MCDC_THIRD_PARTY_SOURCES})
    set_property(
      SOURCE ${_src}
      APPEND
      PROPERTY COMPILE_OPTIONS -fno-coverage-mcdc -fno-coverage-mapping -fno-profile-instr-generate
    )
  endforeach()
endif()
if(REFLOW_USE_LITEHTML)
  target_include_directories(
    ra8_core_hal PUBLIC ${FW_ROOT}/apps/shared_libs/third_party/litehtml/include
  )
  target_link_libraries(ra8_core_hal PUBLIC litehtml gumbo)
endif()
target_compile_features(ra8_core_hal PUBLIC cxx_std_17)

# C++ is enabled here for the optional reflow implementation and C++ tests.
enable_language(CXX)

# ---------------------------------------------------------------------------
# ra8_core_hal_fuzz: slim mirror of ra8_core_hal for the libFuzzer harnesses
# (RA8_FUZZ=ON only; see tests/fuzz/CMakeLists.txt).
#
# ra8_core_hal bundles tests/mocks/src/ra8_fake_mmap.c, whose load-time MAP_FIXED
# constructor maps a peripheral backing window at 0xE0000000. Under
# AddressSanitizer (the Linux fuzz config) that address is inside ASan's
# reserved shadow gap, so the constructor aborts every harness before
# main() (issue #193). This object library is exactly ra8_core_hal MINUS
# ra8_fake_mmap.c: the pure-computation harnesses link it and never run that
# constructor. The two harnesses that genuinely poke peripheral registers
# (fuzz_ra8_canfd, fuzz_ra8_usb_pal) add ra8_fake_mmap.c back on their own link
# line, where it is compiled under ASan and skips the shadow-gap region
# (see ra8_fake_region_mappable() in tests/mocks/src/ra8_fake_mmap.c). ra8_core_hal
# itself is untouched, so the host unit-test build keeps the full mapping.
#
# Sources and per-source properties (LANGUAGE, -w, MC/DC opt-out, the
# stb_truetype -include) are shared with ra8_core_hal because
# set_source_files_properties() is directory-scoped: both libraries pick
# them up from the same source paths. Only built when RA8_FUZZ=ON, so the
# ordinary `just quality::local::test` build never compiles this second object set.
# ---------------------------------------------------------------------------
option(RA8_FUZZ "Build libFuzzer harnesses for parsers (clang only)" OFF)
if(RA8_FUZZ)
  get_target_property(_ra8_core_hal_srcs ra8_core_hal SOURCES)
  list(REMOVE_ITEM _ra8_core_hal_srcs ${CMAKE_CURRENT_SOURCE_DIR}/mocks/src/ra8_fake_mmap.c)
  add_library(ra8_core_hal_fuzz OBJECT ${_ra8_core_hal_srcs})
  ra8_target_enable_project_warnings(ra8_core_hal_fuzz STACK_USAGE_BYTES 0)
  target_compile_options(ra8_core_hal_fuzz PRIVATE)
  target_include_directories(
    ra8_core_hal_fuzz PUBLIC $<TARGET_PROPERTY:ra8_core_hal,INTERFACE_INCLUDE_DIRECTORIES>
  )
  target_compile_features(ra8_core_hal_fuzz PUBLIC cxx_std_17)
  if(REFLOW_USE_LITEHTML)
    target_link_libraries(ra8_core_hal_fuzz PUBLIC litehtml gumbo)
  endif()
endif()

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
# routing, the navigation back-stack and the .ra8app container and admission
# gate are Zig now, so libs/ra8_app/src has no .c left and the
# RA8_APP_SOURCES glob is gone from library_sources.cmake and core_hal.cmake.
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
# no .c left and the RA8_WDT_SUPERVISOR_SOURCES glob is gone from
# library_sources.cmake and core_hal.cmake. The host-only ThreadX shim header
# remains for its focused C coverage test.
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

# Fully migrated: the CTRL1_XL / CTRL2_G bit-field encoders, the
# little-endian sample decoders, the temperature conversion, the FIFO drain AND
# the house-I2C binder are all Zig now, so libs/ra8_lsm6dso/src has
# no .c left, the RA8_LSM6DSO_SOURCES glob is gone from library_sources.cmake
# and core_hal.cmake, and libs/ra8_lsm6dso/src is no longer an include
# directory anywhere. The transport stays a caller-supplied seam, so the host
# suite's canned-response mock substitutes for the I2C / SPI bus exactly as
# before; ra8_lsm6dso_bind_i2c rides in this same archive behind the unchanged
# inc/ra8_lsm6dso.h, which is what the binder cases in test_ra8_lsm6dso.c and
# the imu_lsm6dso_demo app link against.
ra8_add_zig_library(
  NAME
  ra8_lsm6dso
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_lsm6dso
  LIBRARY_NAME
  ra8_lsm6dso
)

# PARTLY migrated (RA8FW-365), and the first board in that state. The dual-core
# shared-RAM descriptor, the USB full-speed port routing, the board bringup
# sequence, the VCOM console stream handle and the clock-profile binding are
# Zig; libs/ra8_board_ek_ra8d2/src still holds the pin/LED/switch core, the
# camera, MIPI panel, audio-USB, touch and PDM layers and src/boot, so
# unlike ra8_board_ra8p1 the RA8_BOARD_EK_RA8D2_SOURCES glob stays and this
# archive links BESIDE those objects rather than replacing them.
#
# No renamed archive here. ra8_board_ra8p1 needed -Dabi-prefix because its
# coverage suite links it alongside the default EK-RA8D2 objects; this layer
# IS the default, so nothing links two copies and a prefix would buy nothing.
#
# The five misc suites (test_ra8_board_ek_ra8d2_{dualcore,usb_port,bringup,
# console_stream,clock_profile}.c) are untouched: they only ever called these
# entry points, never defined them, so they take all eight from this archive
# behind the unchanged inc/ headers.
ra8_add_zig_library(
  NAME
  ra8_board_ek_ra8d2
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_board_ek_ra8d2
  LIBRARY_NAME
  ra8_board_ek_ra8d2
)

# Fully migrated. The per-endpoint packet rings, the two MC/DC-promoted
# predicates, the ra8_usb status -> PAL event translation and the whole public
# ra8_usb_pal.h surface are Zig, so src/ra8_usb_pal.c is gone. The four
# descriptor builders behind inc/ra8_usb_desc.h are Zig, so src/ra8_usb_desc.c
# is gone. The one-call compose facade behind inc/ra8_usb_compose.h (RA8FW-317) is
# Zig, so src/ra8_usb_compose.c is gone and the RA8_USB_PAL_SOURCES glob is
# retired with it. src/ra8_usb_pal_internal.h stays: the host suite includes
# it to drive the two promoted predicates, which the Zig archive exports under
# the same names. The Ring-3 ra8_usb driver stays a link-time seam.
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

# Fully migrated: the transport-neutral facade, the fixed in-memory replay
# backend and the PDM-IF backend are all Zig now, so libs/ra8_audio/src holds
# no .c and the RA8_AUDIO_SOURCES glob is gone from library_sources.cmake and
# core_hal.cmake. The private vtable header src/ra8_audio_internal.h stays,
# because tests/misc/src/test_ra8_audio.c includes it to build its own fake
# backend; the libs/ra8_audio/src include dirs in core_hal.cmake and
# unit_tests.cmake are still needed for it. The PDM HAL and the millisecond
# clock stay link-time externs, so tests/hal/src/test_ra8_pdm.c substitutes
# its fixture exactly as it did for the C.
ra8_add_zig_library(
  NAME
  ra8_audio
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_audio
  LIBRARY_NAME
  ra8_audio
)

# Fully migrated: the lifecycle state machine, the backend-table validation,
# the bounded association wait and the lease rule went first, and the ESP32-C6
# backend followed, so libs/ra8_wifi/src has no .c left and the
# RA8_WIFI_SOURCES entry is gone from library_sources.cmake and core_hal.cmake.
# The backend is src/ra8_wifi_c6link_abi.zig over src/internal/c6link.zig; it
# exports k_ra8_wifi_backend_c6link and ra8_wifi_c6link_setup and calls the
# ra8_c6link entry points the C called, so the c6 host test still needs that
# stack plus the vendored protobuf codec and keeps its own target in
# tests_wifi.cmake. The radio stays a caller-supplied vtable, so the host
# suite's mock backend substitutes for it exactly as before. The two wifi
# targets in tests_wifi.cmake consume ra8_core_hal through
# $<TARGET_OBJECTS:>, which carries no link dependencies, so they link this
# archive by name there rather than through the list below.
# Fully migrated: the OV5640 register protocol, the
# board-qualified VGA DVP scene table, the JPEG overlay writes, the JPEG status
# decode AND the house-I2C binder are all Zig, so libs/ra8_ov5640/src
# has no .c left, the RA8_OV5640_SOURCES glob is gone from
# library_sources.cmake and core_hal.cmake, and libs/ra8_ov5640/src is no
# longer an include directory anywhere. The SCCB transport and the millisecond
# delay stay caller-injected seams, so this archive links against no RA8
# peripheral. ra8_ov5640_bind_i2c rides in this same archive behind the
# unchanged inc/ra8_ov5640.h, which is what the four vectors in
# tests/graphics/src/test_ra8_ov5640_bind.c link against.
ra8_add_zig_library(
  NAME
  ra8_ov5640
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_ov5640
  LIBRARY_NAME
  ra8_ov5640
)

# Fully migrated: the AT line accumulator, the OK / ERROR / +CME ERROR /
# +CMS ERROR / BUSY / NO CARRIER final-result table, the newline-separated
# capture appender and the eight-slot URC dispatch table are all Zig now, so
# libs/ra8_modem_at/src has no .c left and the RA8_MODEM_AT_SOURCES glob is
# gone from library_sources.cmake and core_hal.cmake. The byte transport and
# the millisecond timebase stay caller-injected seams, so the archive links
# against no driver. The eight priv_modem_* helpers declared in
# src/ra8_modem_at_internal.h are driven by the MC/DC suites. They are emitted
# only by a test-configured copy of the archive (-Dtest-helpers=true), which
# carries the public ABI too, so the host tests link that copy alone and the
# plain archive below is the one the target build consumes. Linking both would
# define every ra8_modem_at_* symbol twice, which is why ra8_zig::ra8_modem_at
# is deliberately absent from the two link lists further down.
ra8_add_zig_library(
  NAME
  ra8_modem_at
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_modem_at
  LIBRARY_NAME
  ra8_modem_at
)

set(_ra8_modem_at_test_dir "${CMAKE_CURRENT_BINARY_DIR}/zig_libs/ra8_modem_at_test_helpers")
set(_ra8_modem_at_test_library
    "${_ra8_modem_at_test_dir}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}ra8_modem_at_test_helpers${CMAKE_STATIC_LIBRARY_SUFFIX}"
)
add_custom_target(
  ra8_modem_at_test_helpers_zig_library ALL
  COMMAND "${ZIG_EXECUTABLE}" build -Dtest-helpers=true -Doptimize=Debug --prefix
          "${_ra8_modem_at_test_dir}" --cache-dir "${_ra8_modem_at_test_dir}/cache"
          --global-cache-dir "${_ra8_modem_at_test_dir}/global-cache"
  WORKING_DIRECTORY "${FW_ROOT}/libs/ra8_modem_at"
  BYPRODUCTS "${_ra8_modem_at_test_library}"
  COMMENT "Building test-only ra8_modem_at helper archive"
  VERBATIM
)
add_library(ra8_zig::ra8_modem_at_test_helpers STATIC IMPORTED GLOBAL)
set_target_properties(
  ra8_zig::ra8_modem_at_test_helpers
  PROPERTIES IMPORTED_LOCATION "${_ra8_modem_at_test_library}"
             INTERFACE_INCLUDE_DIRECTORIES "${FW_ROOT}/libs/ra8_modem_at/inc;${FW_ROOT}/libs/ra8_modem_at/src"
)
add_dependencies(ra8_zig::ra8_modem_at_test_helpers ra8_modem_at_test_helpers_zig_library)
link_libraries(ra8_zig::ra8_modem_at_test_helpers)

ra8_add_zig_library(
  NAME
  ra8_wifi
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_wifi
  LIBRARY_NAME
  ra8_wifi
)

# Fully migrated: the fw_if_fs adapter over ra8_io_vfs is Zig now, so
# libs/if_ra8_vfs/src has no .c left and the RA8_IF_RA8_VFS_SOURCES glob is
# gone from library_sources.cmake and core_hal.cmake. The private contracts
# header went with the .c, so libs/if_ra8_vfs/src is no longer an include
# directory anywhere; only inc/ remains. fw_fs_ra8_vfs_init stays the one
# exported symbol, with the namespace, stream and transaction vtables
# file-private exactly as static made them.
ra8_add_zig_library(
  NAME
  if_ra8_vfs
  ZIG_ROOT
  ${FW_ROOT}/libs/if_ra8_vfs
  LIBRARY_NAME
  if_ra8_vfs
)

# libs/ra8_camera is fully migrated: the CEU capture backend
# (src/source_ceu.zig) was its last C translation unit, so the
# RA8_CAMERA_SOURCES glob is gone from library_sources.cmake and
# core_hal.cmake. The two suites that white-boxed the .c now call the
# `priv_cam_ceu_wait_for_frame` and `priv_cam_ceu_capture` seams the archive
# exports, declared in src/ra8_camera_source_ceu_private.h. The
# libs/ra8_camera/src include directory therefore stays: that private header
# and src/ra8_camera_internal.h (the fake source and codec vtables the host
# suites build) both live there.
ra8_add_zig_library(
  NAME
  ra8_camera
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_camera
  LIBRARY_NAME
  ra8_camera
)

# Fully migrated: the portable filesystem facade AND the untrusted-name
# containment policy are both Zig now, so src/ra8_path.c is gone, the
# RA8_IF_SOURCES glob is gone from library_sources.cmake and core_hal.cmake,
# and libs/if/src is no longer an include directory anywhere. The three
# ra8_path_* symbols ride in this same archive behind the unchanged
# inc/ra8_path.h, so no target compiles that .c by path any more.
#
# libs/if/build.zig names the archive libif.a after its directory, which is
# what the app build links, so LIBRARY_NAME follows it. The target keeps the
# fw_if_fs name its consumers link against.
ra8_add_zig_library(
  NAME
  fw_if_fs
  ZIG_ROOT
  ${FW_ROOT}/libs/if
  LIBRARY_NAME
  if
)

# Fully migrated: the RA8 module-to-clock table and the three ops behind
# inc/fw_if_clock_ra8.h are Zig, so fw_if_clock_ra8.c and
# fw_if_clock_ra8_map.c are gone, the RA8_IF_RA8_CGC_SOURCES glob is gone from
# library_sources.cmake and core_hal.cmake, and libs/if_ra8_cgc/src is no
# longer an include directory anywhere. The ops reach ra8_cgc_get_clock_hz,
# ra8_mstp_enable / ra8_mstp_disable and fw_clock_bind as externs resolved at
# the final link, which is why this archive is linked beside fw_if_fs rather
# than standing alone.
ra8_add_zig_library(
  NAME
  if_ra8_cgc
  ZIG_ROOT
  ${FW_ROOT}/libs/if_ra8_cgc
  LIBRARY_NAME
  if_ra8_cgc
)

# Fully migrated: the RA8 GPT32 timer and PWM adapters behind
# inc/fw_if_timer_ra8.h and inc/fw_if_pwm_ra8.h, and the channel-ownership
# table they share, are Zig, so fw_if_timer_ra8.c, fw_if_pwm_ra8.c and
# fw_if_gpt_ra8_claim.{c,h} are gone and libs/if_ra8_gpt/src is no longer an
# include directory. The ops reach ra8_gpt_* and fw_timer_bind / fw_pwm_bind
# as externs resolved at the final link, like if_ra8_cgc.
ra8_add_zig_library(
  NAME
  if_ra8_gpt
  ZIG_ROOT
  ${FW_ROOT}/libs/if_ra8_gpt
  LIBRARY_NAME
  if_ra8_gpt
)

# Fully migrated: the FTL core (init, the presented free-overwrite vtable,
# copy-on-write relocation, reclamation and wear-levelling), the canonical
# checkpoint codec and the mount lifecycle are all Zig, so ra8_ftl.c,
# ra8_ftl_checkpoint.c and ra8_ftl_mount.c are gone and there is no
# RA8_FTL_SOURCES glob left. The archive calls the ra8_io_blockdev_* front
# door, which lives in ra8_core_hal's own objects.
ra8_add_zig_library(
  NAME
  ra8_ftl
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_ftl
  LIBRARY_NAME
  ra8_ftl
)
set_property(
  TARGET ra8_zig::ra8_keyboard
  APPEND
  PROPERTY INTERFACE_LINK_LIBRARIES ra8_zig::ra8_ui
)

# Fully migrated: every hand-written translation unit of the rasteriser is Zig
# now -- the single shared framebuffer binding g_gfx_text_state, the four
# promoted helpers priv_gfx_text_pack_565 / priv_gfx_text_plot / priv_gfx_bpp /
# priv_gfx_format_ok, the fifteen entry points of inc/ra8_gfx.h with the two
# bind forms, the teardown, the packed-gray4 loupe zoom blit and both text
# calls among them, the three inc/ra8_gfx_tone.h calls with their committed
# nominal curve, and the six inc/ra8_gfx_dither.h calls over the committed
# blue-noise mask. Fully migrated: the bundled 8x16 font table
# came over last, so the exported descriptor ra8_gfx_font_8x16 that
# inc/ra8_gfx_font.h declares is in this archive too and the library has no C
# translation unit at all. src/ra8_gfx_internal.h stays as the written record
# of the module-private surface, but no C includes it any more, so the
# libs/ra8_gfx/src include dirs are gone from core_hal.cmake and
# unit_tests.cmake.
ra8_add_zig_library(
  NAME
  ra8_gfx
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_gfx
  LIBRARY_NAME
  ra8_gfx
)

# ra8_core_hal is the OBJECT library every host test links, so an INTERFACE
# link here reaches each test executable that pulls in a migrated library.
# Fully migrated: the protocol core (both CRC generators, command framing,
# the R1/R3/R7 response readers, the bounded waits, the transport gate and
# the CMD0..CMD16 identification sequence) and the block-I/O TU (init/deinit,
# single- and multi-block read/write, erase, the capacity and card-type
# queries, the ra8_fs backend adapter and the SCI Simple-SPI transport
# factory) are both Zig, and the archive owns the sole definition of
# g_sdmmc_spi_state. The archive reaches ra8_sci_spi_*, ra8_gpio_* and
# ra8_pfs_route_peripheral as externs that ra8_core_hal's own objects supply.
# src/ra8_sdmmc_spi_internal.h stays: tests/storage/src/test_ra8_sdmmc_spi_cov.c
# and tests/support/inc/sdmmc_spi_cov_test_util.h include it, so the
# libs/ra8_sdmmc_spi/src include dirs stay too.
ra8_add_zig_library(
  NAME
  ra8_sdmmc_spi
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_sdmmc_spi
  LIBRARY_NAME
  ra8_sdmmc_spi
)

# The BACKEND-AGNOSTIC HALF only: the dispatcher behind inc/ra8_display_pal.h
# (argument guards, the one module-static handle, dispatch through the bound
# vtable) and the page-turn refresh cadence behind
# inc/ra8_display_pal_policy.h are Zig now, so src/ra8_display_pal.c and
# src/ra8_display_pal_policy.c are gone. Both panel backends followed: the
# LCD/GLCDC one is src/ra8_display_pal_lcd_abi.zig over src/internal/lcd.zig,
# and the IT8951 e-paper one is src/ra8_display_pal_eink_abi.zig over
# src/internal/eink.zig. They still drive ra8_glcdc and ra8_epaper inside
# ra8_core_hal, through extern declarations rather than a C include. Nothing
# under libs/ra8_display_pal/src is C any more, so RA8_DISPLAY_PAL_SOURCES is
# empty. The panel stays a caller-supplied vtable,
# so the archive names no controller and the host suite's fake backends
# substitute for one exactly as before. src/ra8_display_pal_internal.h stays
# too: tests/graphics/src/test_ra8_display_pal.c includes it, so the
# libs/ra8_display_pal/src include dirs stay in core_hal.cmake and
# unit_tests.cmake.
ra8_add_zig_library(
  NAME
  ra8_display_pal
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_display_pal
  LIBRARY_NAME
  ra8_display_pal
)

# Fully migrated: the parsing/validation cluster, the orchestration state
# machine (which owns g_ra8_ota_cfg, g_ra8_ota_state, g_ra8_ota_initialized and
# g_ra8_ota_buf) and the verify cluster are all this archive, so
# libs/ra8_ota/src carries no C at all. The weak ra8_ota_system_reset_hook
# default is a separate archive member, so a strong definition in the image (or
# in tests/misc/src/test_ra8_ota.c) still overrides it. src/ra8_ota_internal.h
# stays -- three C suites include it -- so the libs/ra8_ota/src include dirs
# stay in core_hal.cmake and unit_tests.cmake.
ra8_add_zig_library(
  NAME
  ra8_ota
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_ota
  LIBRARY_NAME
  ra8_ota
)

# Fully migrated: the exact decimal -> binary64 conversion behind inc/ra8_num.h
# is Zig, so libs/ra8_num/src has no .c left. This library was never in the
# RA8_*_SOURCES glob set at all: three test targets named
# src/ra8_num_decimal.c directly by path, so the archive replaced those
# by-path references in unit_tests.cmake rather than a glob. The archive
# resolves every symbol it names: the conversion is pure arithmetic with no
# driver call, no MMIO and no libc conversion.
ra8_add_zig_library(
  NAME
  ra8_num
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_num
  LIBRARY_NAME
  ra8_num
)

# Fully migrated: the one encode-then-write bridge TU behind
# inc/ra8_camera_stream.h is Zig, so libs/ra8_camera_io/src has no .c left and
# the RA8_CAMERA_IO_SOURCES glob is gone from library_sources.cmake and
# core_hal.cmake. The private src include dir went with it: no test includes a
# private header from this library. The archive deliberately leaves
# ra8_camera_codec_encode and ra8_io_stream_write unresolved, exactly as the C
# TU did; ra8_core_hal supplies both.
ra8_add_zig_library(
  NAME
  ra8_camera_io
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_camera_io
  LIBRARY_NAME
  ra8_camera_io
)

# Fully migrated: the secure-boot sequence behind inc/ra8_tz_secure_boot.h,
# the SAU region partition, the IPC attribution encoder behind
# inc/ra8_tz_ipc_attr.h, the board partition map behind inc/ra8_tz_partition.h
# and the PSAR gate behind inc/ra8_tz_psar.h are all Zig now, so
# libs/ra8_tz_secure_boot/src has no .c left and the RA8_TZ_SECURE_BOOT_SOURCES
# glob is gone from library_sources.cmake and core_hal.cmake. The private src
# include dir went with the .c files: no test includes a private header from
# this library. ns/ra8_ns_rot_header.zig (Zig since RA8FW-639) is not part of
# this library at all: ra8_add_ns_image.cmake builds it into the Non-Secure
# image as its root-of-trust header data.
ra8_add_zig_library(
  NAME
  ra8_tz_secure_boot
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_tz_secure_boot
  LIBRARY_NAME
  ra8_tz_secure_boot
)

# ra8_dfu's boot, launch and program logic. The full ra8_dfu archive also
# carries the USB host DFU driver, whose ra8_usb_host_* seam has no host
# implementation, so the suites link the ra8_dfu_boot archive the same
# build.zig installs without it (see libs/ra8_dfu/src/boot_root.zig).
ra8_add_zig_library(
  NAME
  ra8_dfu_boot
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_dfu
  LIBRARY_NAME
  ra8_dfu_boot
)

# The root of trust is Zig only: image verification against the provisioned
# root key and the anti-rollback counter. Its port left it unregistered here,
# so test_ra8_root_of_trust could not link. libs/ra8_rot has no inc/; its
# headers (ra8_rot.h, ra8_dfu_antirollback.h) still live in libs/ra8_dfu/inc.
ra8_add_zig_library(
  NAME
  ra8_rot
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_rot
  LIBRARY_NAME
  ra8_rot
)
set_target_properties(
  ra8_zig::ra8_rot PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${FW_ROOT}/libs/ra8_dfu/inc"
)

# Fully migrated: the bounded XML emitter behind inc/ra8_xml_writer.h is Zig,
# so libs/ra8_xml/src has no .c left. This library sat in BOTH wiring worlds:
# RA8_XML_WRITER_SOURCES was globbed in library_sources.cmake and by-path
# references compiled the TU directly into other builds. The archive replaces
# the glob entry and every by-path reference. It resolves every symbol it names:
# the emitter is pure string construction over caller-owned storage with no
# allocation, no driver call and no MMIO.
ra8_add_zig_library(
  NAME
  ra8_xml
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_xml
  LIBRARY_NAME
  ra8_xml
)

# Fully migrated: the URL and peer-address safety policy behind
# inc/ra8_net_urlguard.h is Zig, so libs/ra8_net_policy/src has no .c left.
# Like ra8_xml this library sat in BOTH wiring worlds: RA8_NET_POLICY_SOURCES
# was globbed in library_sources.cmake and by-path references compiled
# the TU directly into other builds. The archive replaces the glob entry and
# every by-path reference. It
# resolves every symbol it names: the policy is pure lexical and numeric work
# over caller-owned storage, with no allocation, no name resolution and no
# network call.
ra8_add_zig_library(
  NAME
  ra8_net_policy
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_net_policy
  LIBRARY_NAME
  ra8_net_policy
)

# Fully migrated: the one image-decoder seam behind inc/ra8_imgdec.h,
# inc/ra8_imgdec_backend.h, inc/ra8_imgdec_mux.h, inc/ra8_imgdec_name.h and
# inc/ra8_imgdec_scratch.h is Zig, so libs/ra8_imgdec/src has no .c left.
# This library had the widest wiring surface of the port so far: the
# RA8_IMGDEC_SOURCES glob in library_sources.cmake plus twenty-two by-path
# references across the app source rules, the WebP vendor rule, cbz2jof
# and the host tools. The archive
# replaces every one of them.
#
# It does NOT resolve every symbol it names: ra8_arena_carve and
# ra8_arena_remaining are still C in ra8_mem, one ring down, and only
# src/ra8_imgdec_abi.zig names them. Everything under src/internal reaches the
# arena through an injected seam, which is what lets the whole of the decision
# logic run in the host suite.
# Fully migrated: the PSA crypto facade behind inc/ra8_psa_crypto.h is Zig,
# so libs/ra8_psa_crypto/src has no .c left. The library is one seam with two
# backends chosen at comptime from build_config.off_target: a deterministic
# fake (xorshift32 keystream, truncating at the 256-byte scratch) for the host
# and off-target builds, and a binding to the vendored tf-psa-crypto for ARM.
#
# src/ra8_psa_crypto_internal.h is deliberately RETAINED. It is not an
# implementation, it is the contract tests/security/src/test_ra8_psa_crypto_*.c
# read for struct ra8_psa_key_handle and k_ra8_psa_fake_scratch_bytes, and
# those two C suites stay C so they keep testing the archive from the outside.
ra8_add_zig_library(
  NAME
  ra8_psa_crypto
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_psa_crypto
  LIBRARY_NAME
  ra8_psa_crypto
)

# Fully migrated: the Mbed TLS facade behind inc/ra8_tls.h is Zig, so
# libs/ra8_tls/src has no .c left. Same two-backend shape as ra8_psa_crypto,
# chosen at comptime from build_config.off_target: a loopback stand-in that
# drives the transport seam for the host and off-target builds, and a binding
# to the vendored Mbed TLS for ARM.
#
# The three C suites (tests/wireless/src/test_ra8_tls.c, test_ra8_tls_net.c,
# tests/fuzz/src/fuzz_ra8_tls.c) stay C on purpose: they only ever included
# inc/ra8_tls.h, so they keep testing the archive from the outside.
ra8_add_zig_library(
  NAME
  ra8_tls
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_tls
  LIBRARY_NAME
  ra8_tls
)

ra8_add_zig_library(
  NAME
  ra8_imgdec
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_imgdec
  LIBRARY_NAME
  ra8_imgdec
)

# Fully migrated: the key vault behind inc/key_vault.h,
# the entropy read behind src/secure_trng_internal.h, the OTA bank commit
# behind inc/ota_commit.h, the AES-CMAC behind src/sec_cmac_internal.h and the
# sealed-key import behind src/key_import_internal.h are Zig now, so
# libs/ra8_secure_app/src has no .c left and the RA8_SECURE_APP_SOURCES glob is
# gone from library_sources.cmake and core_hal.cmake.
#
# The four headers are unchanged and are still the membrane:
# tests/security/src/test_secure_app_sec_cmac.c pins the archive to the
# published NIST SP 800-38B vectors through src/sec_cmac_internal.h, and
# test_secure_app_key_import.c drives the sealed-blob format and the handle
# allocator through src/key_import_internal.h.
ra8_add_zig_library(
  NAME
  ra8_secure_app
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_secure_app
  LIBRARY_NAME
  ra8_secure_app
)

# Fully migrated: the fixed-cell slab allocator, the byte-stream adapter
# over the page cache, the page cache itself, the glyph atlas, the image-tile
# cache and the object-source registry are Zig now, so
# libs/ra8_mem/src/ra8_slab.c, src/ra8_vmem.c, src/ra8_vmem_stream.c,
# src/ra8_glyph_atlas.c, src/ra8_tile_cache.c and src/ra8_vsource.c are gone.
# Their headers are unchanged and are still the membrane, so
# tests/core/src/test_ra8_slab.c, tests/core/src/test_ra8_vsource.c,
# tests/graphics/src/test_ra8_tile_cache.c, tests/mocks/src/test_app_mem_subsystem.c,
# libs/ra8_io/src/ra8_io_blockdev_vsource.c, the book/EPUB/comic/manga suites
# and rabook_import link this archive without knowing the bodies moved.
#
# The source registry adds no extern of its own: a paged object reads through a
# callback pointer it is handed, not a link-time symbol.
#
# The cache engine is Zig now, so this archive has NO undefined ra8_mem symbol
# left: ra8_keycache_init/get/prefetch/put/stats are exported from here, the
# three typed facades over the engine (the page cache, the glyph atlas, the
# tile cache) call it directly inside the archive, and the stream adapter
# calls the page cache the same way. tools/glyph_bench, tools/cache_bench and
# tools/reader_vmem each compiled src/ra8_vmem.c and src/ra8_keycache.c by
# absolute path only to satisfy those externs; none of them does now, and
# tools/rabook_viewer no longer partitions this directory at all.
#
# tests/core/src/test_ra8_vmem.c and apps/shared_libs/book's huge-book suite are
# untouched and now exercise the Zig page cache through inc/ra8_vmem.h.
#
# The init-time bump arena was the last C translation unit here and it is Zig
# now, so libs/ra8_mem/src has no .c left and this archive exports ra8_arena_*
# like every other ra8_mem symbol. What blocked that was never the port, it was
# the double definition: tools/rabook_viewer, tools/rabook_imagepack,
# a host app and cmake/ra8_webp_vendor.cmake each COMPILED
# libs/ra8_mem/src/ra8_arena.c by absolute path instead of linking the library,
# so exporting the symbols would have defined each of them twice. All four now
# link ra8_zig::ra8_mem, which is the form that dedupes: CMake collapses a
# repeated imported target where it cannot collapse a repeated source.
#
# inc/ra8_arena.h is unchanged and is still the membrane:
# tests/misc/src/test_ra8_arena.c stays C and tests the archive from outside,
# alongside the Zig unit suite in src/arena_test.zig.
#
# The ra8_mem *.c glob in cmake/ra8_app/sources.cmake is empty from here on,
# which is the shape: its transitive reflow/book path already registers
# this archive explicitly and judges the flip on build.zig rather than on the
# glob, so an empty glob is correct there rather than a missing library.
ra8_add_zig_library(
  NAME
  ra8_mem
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_mem
  LIBRARY_NAME
  ra8_mem
)

# Fully migrated. The last C translation unit, ra8_widget.c (the flat op core:
# rect maths, invalidate, damage, the dispatch every widget routes through and
# render_dirty), is Zig now as src/widget_core_abi.zig, and it is deleted along
# with the private header src/ra8_widget_internal.h that published the three
# RA8_PRIV paint helpers to it. Those helpers are plain Zig calls inside this
# archive now, so the private header has no callers left. libs/ra8_widget/src
# carries no C at all, the RA8_WIDGET_SOURCES glob in library_sources.cmake is
# gone, and every widget entry point, vtable and core op is published C ABI out
# of this one archive.
ra8_add_zig_library(
  NAME
  ra8_widget
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_widget
  LIBRARY_NAME
  ra8_widget
)

# Partially migrated: the TLV envelope the co-processor's serial endpoint
# speaks is Zig now and libs/ra8_c6link/src/ra8_c6link_tlv.c is gone. The rest
# of the library is still C and keeps calling the two priv_c6link_tlv_*
# symbols through the unchanged src/ra8_c6link_internal.h declarations, so the
# RA8_C6LINK_SOURCES glob in tests_c6link.cmake stays and simply stops seeing
# the deleted file. Every C file left behind carries a row in
# .github/zig-parallel-tree-allowlist.tsv.
ra8_add_zig_library(
  NAME
  ra8_c6link
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_c6link
  LIBRARY_NAME
  ra8_c6link
)

# Partly migrated (RA8FW-497): ported ra8_hal units, beside its remaining C.
ra8_add_zig_library(
  NAME
  ra8_hal
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_hal
  LIBRARY_NAME
  ra8_hal
)

# Partly migrated (RA8FW-654, RA8FW-697): ported ra8_io units (the log sink
# adapter, the SDRAM block-device backend), beside its remaining C.
ra8_add_zig_library(
  NAME
  ra8_io
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_io
  LIBRARY_NAME
  ra8_io
)

include(${CMAKE_CURRENT_LIST_DIR}/zig_link_order.cmake)

# Fully migrated: the `ra8_imgdec` backend (inc/ra8_jpeg_imgdec.h) moved
# first, then the encoder, then the marker walk, the whole-buffer
# decoder and the streaming driver. libs/ra8_jpeg/src holds no .c at
# all now, so the RA8_JPEG_SOURCES glob in library_sources.cmake matches
# nothing and ra8_core_hal gets the codec only from this archive. The
# hand-authored C23 headers under libs/ra8_jpeg/inc are unchanged, so every C
# suite that includes them is untouched.
ra8_add_zig_library(
  NAME
  ra8_jpeg
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_jpeg
  LIBRARY_NAME
  ra8_jpeg
)
link_libraries(ra8_zig::ra8_jpeg)

# ra8_core is partially migrated and ships TWO archives, because one of its
# two Zig seams exports bare libc names and the other does not.
#
# ra8_zig::ra8_core is the general one (libra8_core_zig.a). It holds every
# ported TU whose exported names are ordinary ra8_* ones: the pin-claim
# validator so far. Nothing in a host test binary defines those, so it is
# link_libraries()'d like the other migrated libraries and the untouched C
# suites reach the Zig object code with no test edit. New ra8_core slices
# belong in this archive.
#
# The freestanding runtime primitives are the exception and stay in
# their own archive (libra8_core.a), which is deliberately NOT registered
# here. Its exported names are the bare standard ones an image needs (memcpy,
# memset, strlen, abs), and a host test binary already has a real libc
# defining every one of them.
#
# tests/core/src/test_ra8_freestanding.c is the one suite that has to reach
# those implementations, and it does it the way the C did: it defines
# RA8_TEST_FREESTANDING, whose block in libs/ra8_core/inc/ra8_freestanding.h
# rewrites its bare calls to ra8_memset / ra8_strlen / ra8_abs. So the suite
# links a copy of the freestanding archive built with -Dabi-prefix=ra8_,
# which exports exactly those names and collides with nothing.
# unit_tests.cmake attaches it to that one target; it is not
# link_libraries()'d, because no other test should pick these symbols up.
# That archive carries no pin_validator symbols, so a target that links both
# gets one definition of each, not two.
ra8_add_zig_library(
  NAME
  ra8_core
  ZIG_ROOT
  ${FW_ROOT}/libs/ra8_core
  LIBRARY_NAME
  ra8_core_zig
)
link_libraries(ra8_zig::ra8_core)

set(_ra8_core_prefixed_dir "${CMAKE_CURRENT_BINARY_DIR}/zig_libs/ra8_core_freestanding_prefixed")
set(_ra8_core_prefixed_library
    "${_ra8_core_prefixed_dir}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}ra8_core${CMAKE_STATIC_LIBRARY_SUFFIX}"
)
add_custom_target(
  ra8_core_freestanding_prefixed_zig_library ALL
  COMMAND "${ZIG_EXECUTABLE}" build -Dabi-prefix=ra8_ -Doptimize=Debug --prefix
          "${_ra8_core_prefixed_dir}" --cache-dir "${_ra8_core_prefixed_dir}/cache"
          --global-cache-dir "${_ra8_core_prefixed_dir}/global-cache"
  WORKING_DIRECTORY "${FW_ROOT}/libs/ra8_core"
  BYPRODUCTS "${_ra8_core_prefixed_library}"
  COMMENT "Building ra8_ prefixed ra8_core archive for test_ra8_freestanding"
  VERBATIM
)
add_library(ra8_zig::ra8_core_freestanding_prefixed STATIC IMPORTED GLOBAL)
set_target_properties(
  ra8_zig::ra8_core_freestanding_prefixed
  PROPERTIES IMPORTED_LOCATION "${_ra8_core_prefixed_library}"
             INTERFACE_INCLUDE_DIRECTORIES "${FW_ROOT}/libs/ra8_core/inc"
)
add_dependencies(ra8_zig::ra8_core_freestanding_prefixed ra8_core_freestanding_prefixed_zig_library)

# Fully migrated: libs/ra8_board_ra8p1/src holds no .c for the board layer, only
# src/boot/, which ra8_add_app's boot fallback still compiles as C.
#
# This archive is NOT added to the global link_libraries() set below. The RA8P1
# BSP exports the same substitutable names as the EK-RA8D2 layer that every
# host test already links, so only the focused coverage suite takes it, and it
# takes the -Dabi-prefix=ra8p1_test_ build to keep the two apart.
set(_ra8p1_prefixed_dir "${CMAKE_CURRENT_BINARY_DIR}/zig_libs/ra8_board_ra8p1_prefixed")
set(_ra8p1_prefixed_library
    "${_ra8p1_prefixed_dir}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}ra8_board_ra8p1${CMAKE_STATIC_LIBRARY_SUFFIX}"
)
add_custom_target(
  ra8_board_ra8p1_prefixed_zig_library ALL
  COMMAND "${ZIG_EXECUTABLE}" build -Dabi-prefix=ra8p1_test_ -Doptimize=Debug --prefix
          "${_ra8p1_prefixed_dir}" --cache-dir "${_ra8p1_prefixed_dir}/cache"
          --global-cache-dir "${_ra8p1_prefixed_dir}/global-cache"
  WORKING_DIRECTORY "${FW_ROOT}/libs/ra8_board_ra8p1"
  BYPRODUCTS "${_ra8p1_prefixed_library}"
  COMMENT "Building ra8p1_test_ prefixed ra8_board_ra8p1 archive for test_ra8_board_ra8p1_cov"
  VERBATIM
)
add_library(ra8_zig::ra8_board_ra8p1_prefixed STATIC IMPORTED GLOBAL)
set_target_properties(
  ra8_zig::ra8_board_ra8p1_prefixed
  PROPERTIES IMPORTED_LOCATION "${_ra8p1_prefixed_library}"
             INTERFACE_INCLUDE_DIRECTORIES "${FW_ROOT}/libs/ra8_board_ra8p1/inc"
)
add_dependencies(ra8_zig::ra8_board_ra8p1_prefixed ra8_board_ra8p1_prefixed_zig_library)

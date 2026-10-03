# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Link order for the migrated Zig archives, split out of zig_libraries.cmake
# (RA8FW-497) so registration and ordering each stay under the size cap.
# Included from zig_libraries.cmake right after the last ra8_add_zig_library
# that these lists name; the text is unchanged from its old home.

target_link_libraries(
  ra8_core_hal
  PUBLIC ra8_zig::ra8_box
         ra8_zig::ra8_power_profile
         ra8_zig::ra8_epd_cal
         ra8_zig::ra8_touch_cal
         ra8_zig::ra8_devcfg
         ra8_zig::ra8_batt
         ra8_zig::ra8_ui
         ra8_zig::ra8_app
         ra8_zig::ra8_wdt_supervisor
         ra8_zig::ra8_mpu
         ra8_zig::ra8_c6link
         ra8_zig::ra8_net_pal
         ra8_zig::ra8_lsm6dso
         ra8_zig::ra8_board_ek_ra8d2
         ra8_zig::ra8_hal
         ra8_zig::ra8_usb_pal
         ra8_zig::ra8_keyboard
         ra8_zig::ra8_audio
         ra8_zig::ra8_wifi
         ra8_zig::ra8_ov5640
         ra8_zig::if_ra8_vfs
         ra8_zig::ra8_camera
         ra8_zig::fw_if_fs
         ra8_zig::if_ra8_cgc
         ra8_zig::if_ra8_gpt
         ra8_zig::if_ra8_gpt
         ra8_zig::ra8_ftl
         ra8_zig::ra8_sdmmc_spi
         ra8_zig::ra8_display_pal
         ra8_zig::ra8_ota
         ra8_zig::ra8_tz_secure_boot
         ra8_zig::ra8_camera_io
         ra8_zig::ra8_xml
         ra8_zig::ra8_net_policy
         ra8_zig::ra8_imgdec
         ra8_zig::ra8_psa_crypto
         ra8_zig::ra8_tls
         ra8_zig::ra8_secure_app
         ra8_zig::ra8_mem
         ra8_zig::ra8_widget
         ra8_zig::ra8_gfx
         ra8_zig::ra8_gfx
)

link_libraries(ra8_zig::ra8_batt)

# Object-library consumers do not inherit the core target's link interface;
# attach migrated archives at directory scope so every host test links them.
# Order matters: the linker reads each archive once, left to right, so an
# archive goes after every archive that calls into it (ra8_box after
# ra8_widget, ra8_gfx after its ra8_widget caller).
link_libraries(
  ra8_zig::ra8_power_profile
  ra8_zig::ra8_epd_cal
  ra8_zig::ra8_touch_cal
  ra8_zig::ra8_devcfg
  ra8_zig::ra8_ui
  ra8_zig::ra8_app
  ra8_zig::ra8_wdt_supervisor
  ra8_zig::ra8_mpu
  ra8_zig::ra8_c6link
  ra8_zig::ra8_net_pal
  ra8_zig::ra8_lsm6dso
  ra8_zig::ra8_board_ek_ra8d2
  ra8_zig::ra8_hal
  ra8_zig::ra8_usb_pal
  ra8_zig::ra8_keyboard
  ra8_zig::ra8_audio
  ra8_zig::ra8_wifi
  ra8_zig::ra8_ov5640
  ra8_zig::if_ra8_vfs
  ra8_zig::ra8_camera
  ra8_zig::fw_if_fs
  ra8_zig::if_ra8_cgc
  ra8_zig::if_ra8_gpt
  ra8_zig::ra8_ftl
  ra8_zig::ra8_sdmmc_spi
  ra8_zig::ra8_display_pal
  ra8_zig::ra8_ota
  ra8_zig::ra8_tz_secure_boot
  ra8_zig::ra8_rot
  ra8_zig::ra8_dfu_boot
  ra8_zig::ra8_camera_io
  ra8_zig::ra8_xml
  ra8_zig::ra8_net_policy
  ra8_zig::ra8_imgdec
  ra8_zig::ra8_psa_crypto
  ra8_zig::ra8_tls
  ra8_zig::ra8_secure_app
  ra8_zig::ra8_mem
  ra8_zig::ra8_widget
  ra8_zig::ra8_box
  ra8_zig::ra8_gfx
)

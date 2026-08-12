# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# libs/ra8_display_pal -- backend-agnostic display abstraction layer.
#
# Today there are two backends:
#   1. LCD (ra8_glcdc + EK-RA8D2 panel) -- wraps the existing GLCDC
#      bring-up sequence into one display_init call.
#   2. E-ink (IT8951) -- stub returning k_ra8_err_not_supported from
#      flush/get_framebuffer until the hardware driver lands.
#
# Apps swap backends by changing one line of their display_cfg_t:
#
#     .iface = &k_display_backend_lcd_ra8_glcdc,
#     ...
#     .iface = &k_display_backend_eink_it8951,
#
# Each consumer (app or test) globs libs/ra8_display_pal/src/*.c
# directly, the same way ra8_ota / ra8_net_pal / ra8_usb_pal are wired
# in. This file is intentionally a no-op so the existing per-app
# CMakeLists.txt + tests/CMakeLists.txt patterns keep working.
#
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.

#
# libs/ra_display_pal -- backend-agnostic display abstraction layer.
#
# Today there are two backends:
#   1. LCD (ra_glcdc + EK-RA8D2 panel) -- wraps the existing GLCDC
#      bring-up sequence into one display_init call.
#   2. E-ink (IT8951) -- stub returning k_ra_err_not_supported from
#      flush/get_framebuffer until the hardware driver lands.
#
# Apps swap backends by changing one line of their display_cfg_t:
#
#     .iface = &k_display_backend_lcd_ra_glcdc,
#     ...
#     .iface = &k_display_backend_eink_it8951,
#
# Each consumer (app or test) globs libs/ra_display_pal/src/*.c
# directly, the same way ra_ota / ra_net_pal / ra_usb_pal are wired
# in. This file is intentionally a no-op so the existing per-app
# CMakeLists.txt + tests/CMakeLists.txt patterns keep working.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.

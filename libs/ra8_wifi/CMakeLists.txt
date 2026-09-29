# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Intentionally a no-op, like ra8_c6link / ra8_display_pal / ra8_net_pal.
# Each consumer (app or test) globs the sources it needs directly and adds
# libs/ra8_wifi/inc to its include path:
#
#   * the pure facade is Zig (src/ra8_wifi_abi.zig), host-testable with a mock
#     backend, and arrives as the archive ra8_zig::ra8_wifi rather than in
#     ra8_core_hal; test_ra8_wifi.c drives it standalone;
#   * the ESP32-C6 backend is Zig too (src/ra8_wifi_c6link_abi.zig over
#     src/internal/c6link.zig) and rides ra8_c6link, so a cross app still says
#     `LIBS ra8_wifi ra8_c6link` and `USES esp_hosted`, and the host build
#     links the archive alongside the vendored codec in
#     tests/cmake/tests_wifi.cmake -- the same shape ra8_c6link uses.

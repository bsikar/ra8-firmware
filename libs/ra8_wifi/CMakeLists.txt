# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Intentionally a no-op, like ra8_c6link / ra8_display_pal / ra8_net_pal.
# There is nothing here to compile: this library is entirely Zig now, built by
# build.zig and consumed as the archive ra8_zig::ra8_wifi. Each consumer (app
# or test) links that archive and adds libs/ra8_wifi/inc to its include path:
#
#   * the pure facade is host-testable with a mock backend, so test_ra8_wifi.c
#     drives the archive standalone (tests/cmake/zig_libraries.cmake links it);
#   * the ESP32-C6 backend is its own object inside the same archive, and it is
#     the only part that references ra8_c6link, so `ld` pulls that member only
#     for a target naming k_ra8_wifi_backend_c6link. A cross app still says
#     `LIBS ra8_wifi ra8_c6link` and `USES esp_hosted`, and the host build links
#     it alongside the vendored codec in tests/cmake/tests_wifi.cmake -- the
#     same shape ra8_c6link uses.

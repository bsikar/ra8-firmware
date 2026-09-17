# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Intentionally a no-op, like ra8_c6link / ra8_display_pal / ra8_net_pal.
# Each consumer (app or test) globs the sources it needs directly and adds
# libs/ra8_wifi/inc to its include path:
#
#   * the pure facade (src/ra8_wifi.c) is host-testable with a mock backend, so
#     the host test build compiles it into ra8_core_hal (tests/cmake/
#     library_sources.cmake) and test_ra8_wifi.c drives it standalone;
#   * the ESP32-C6 backend (src/ra8_wifi_c6link.c) rides ra8_c6link, so a cross
#     app says `LIBS ra8_wifi ra8_c6link` and `USES esp_hosted`, and the host
#     build compiles it alongside the vendored codec in
#     tests/cmake/tests_wifi.cmake -- the same shape ra8_c6link uses.

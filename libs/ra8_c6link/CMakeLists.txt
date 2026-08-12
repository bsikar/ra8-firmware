# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# libs/ra8_c6link/CMakeLists.txt
#
# Intentionally a no-op, like ra8_io / ra8_display_pal / ra8_net_pal.
# Each consumer (app or test) globs libs/ra8_c6link/src/*.c directly and adds
# libs/ra8_c6link/inc to its include path:
#
#   * cross-compiled apps say `LIBS ra8_c6link` in their ra8_add_app() call,
#     and `USES esp_hosted` for the vendored codec this library encodes with;
#   * the host test build wires the sources into two standalone targets in
#     tests/cmake/tests_c6link.cmake, because the vendored protobuf codec has
#     to be compiled alongside them and ra8_core_hal does not carry it.
#

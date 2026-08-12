# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# libs/ra8_io/CMakeLists.txt
#
# Intentionally a no-op, like ra8_display_pal / ra8_net_pal / ra8_usb_pal.
# Each consumer (app or test) globs libs/ra8_io/src/*.c directly and adds
# libs/ra8_io/inc to its include path. The host test build wires the
# sources into the shared ra8_core_hal object library in tests/CMakeLists.txt;
# cross-compiled apps pick them up through cmake/ra8_add_app.cmake.
#

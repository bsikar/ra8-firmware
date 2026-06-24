# libs/ra_io/CMakeLists.txt
#
# Intentionally a no-op, like ra_display_pal / ra_net_pal / ra_usb_pal.
# Each consumer (app or test) globs libs/ra_io/src/*.c directly and adds
# libs/ra_io/inc to its include path. The host test build wires the
# sources into the shared ra_core_hal object library in tests/CMakeLists.txt;
# cross-compiled apps pick them up through cmake/ra_add_app.cmake.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

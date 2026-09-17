# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Intentionally a no-op, like ra8_camera and ra8_io. Firmware apps list
# `ra8_audio` in ra8_add_app(... LIBS ...) and the shared app recipe discovers
# its src/ plus inc/. The host test build mirrors that discovery in
# tests/cmake/library_sources.cmake.

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Intentionally a no-op, like ra8_io and ra8_display_pal. Firmware apps list
# `ra8_camera` in ra8_add_app(... LIBS ...) and the shared app recipe globs its
# src/ plus inc/. The host test build mirrors that discovery in
# tests/cmake/library_sources.cmake. The optional `ra8_camera_io` bridge is a
# separate library so capture-only consumers do not inherit `ra8_io`.

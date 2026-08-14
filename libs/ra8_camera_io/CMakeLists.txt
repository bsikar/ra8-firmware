# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Intentionally a no-op. Firmware apps that need the camera-to-I/O bridge list
# `ra8_camera_io`, `ra8_camera`, and `ra8_io` in ra8_add_app(... LIBS ...); the
# shared app recipe discovers this library's src/ and inc/ directories.

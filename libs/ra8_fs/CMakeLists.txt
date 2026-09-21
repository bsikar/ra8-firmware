# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# libs/ra8_fs -- minimal FAT12/FAT16/FAT32 filesystem adapter on top of
# block-device backends (ra8_sdhi, ra8_usb_pmsc).
#
# Picked up automatically by tests/CMakeLists.txt globbing. No board-side
# firmware example currently links it directly, so this file exists as a
# placeholder and to document scope. When an example starts to depend on
# ra8_fs, it does NOT need anything added here: name the library in the
# app's ra8_add_app(... LIBS ra8_fs), which cmake/ra8_app/sources.cmake
# resolves by globbing libs/<lib>/src/*.c and appending libs/<lib>/inc. That
# is how every library consumer in this tree actually links.
#
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.

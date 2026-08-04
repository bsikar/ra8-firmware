# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# libs/ra8_fs -- minimal FAT12/FAT16/FAT32 filesystem adapter on top of
# block-device backends (ra8_sdhi, ra8_usb_pmsc).
#
# Picked up automatically by tests/CMakeLists.txt globbing. No board-side
# firmware example currently links it directly, so this file exists as a
# placeholder and to document scope. When an example starts to depend on
# ra8_fs, wire up an OBJECT or INTERFACE library here following the
# pattern in libs/ra8_tls/CMakeLists.txt.
#
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.

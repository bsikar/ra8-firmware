#
# libs/ra_fs -- minimal FAT12/FAT16/FAT32 filesystem adapter on top of
# block-device backends (ra_sdhi, ra_usb_pmsc).
#
# Picked up automatically by tests/CMakeLists.txt globbing. No board-side
# firmware example currently links it directly, so this file exists as a
# placeholder and to document scope. When an example starts to depend on
# ra_fs, wire up an OBJECT or INTERFACE library here following the
# pattern in libs/ra_net/CMakeLists.txt.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.

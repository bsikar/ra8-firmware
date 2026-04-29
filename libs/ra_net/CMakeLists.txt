#
# libs/ra_net -- minimal lwIP-style TCP/IP stack adapter on top of
# ra_net_pal descriptor rings.
#
# This library is currently picked up via tests/CMakeLists.txt globbing
# (the unit tests build all libs/* sources together) and via per-app
# example CMakeLists.txt that reference libs/ra_net/inc + the source
# tree directly. No board-side firmware target depends on it yet, so
# this file exists as a placeholder for future add_library() use.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Intentionally empty -- consumers (tests, examples) reach into
# inc/ and src/ directly. When an app starts to depend on ra_net,
# wire up an OBJECT or INTERFACE library here.

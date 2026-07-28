# libs/ra8_cache_store/CMakeLists.txt
#
# Intentionally a no-op, like ra8_ftl / ra8_io / ra8_mem. Each consumer (app or
# test) globs libs/ra8_cache_store/src/*.c directly and adds
# libs/ra8_cache_store/inc to its include path. The host test build wires the
# sources into the shared ra8_core_hal object library in tests/CMakeLists.txt
# (with LevelX standalone + the RAM NOR fake); cross-compiled apps pick
# them up through cmake/ra8_add_app.cmake plus `USES levelx`.
#
# ra8_cache_store depends only on ra8_core (ra8_err/ra8_check/ra8_log) and the
# vendored LevelX NOR logical-sector API (lx_nor_flash_*); it never touches the
# LevelX ThreadX mutex fields, so it compiles under both LX_STANDALONE_ENABLE
# (host tests) and the ThreadX LevelX build (target apps).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

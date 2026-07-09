# libs/ra_cache_store/CMakeLists.txt
#
# Intentionally a no-op, like ra_ftl / ra_io / ra_mem. Each consumer (app or
# test) globs libs/ra_cache_store/src/*.c directly and adds
# libs/ra_cache_store/inc to its include path. The host test build wires the
# sources into the shared ra_core_hal object library in tests/CMakeLists.txt
# (with LevelX standalone + the RAM NOR simulator); cross-compiled apps pick
# them up through cmake/ra_add_app.cmake plus `USES levelx`.
#
# ra_cache_store depends only on ra_core (ra_err/ra_check/ra_log) and the
# vendored LevelX NOR logical-sector API (lx_nor_flash_*); it never touches the
# LevelX ThreadX mutex fields, so it compiles under both LX_STANDALONE_ENABLE
# (host tests) and the ThreadX LevelX build (target apps).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/ra8_production_build.cmake -- the shipping-image marker (issue #1085).
#
# `RA8_PRODUCTION_BUILD` declares that the image being configured is one that
# ships. It arms the fail-closed guards that only a shipping image may trip.
# Today that is the `#error` in libs/ra8_epd_cal/src/ra8_epd_cal.c, which
# refuses to compile a bench VCOM (`-DRA8_BENCH_VCOM_MV=<mv>`) into a panel
# driver that reaches a customer -- the whole reason that module exists.
#
# Before this file the marker was named by that guard and by the ra8_epd_cal
# header's safety claim, and defined by NOTHING: no option, no preset, no
# workflow, no script. The guard could not fire in any configuration that
# exists, so a release build with a bench VCOM on the command line compiled
# clean and shipped the bench value.
#
# OFF by default, so a bench or dev build keeps its escape hatches unchanged
# and no compile sees the define. The `ra8d2-release` preset turns it ON, so
# the guard is armed by the configure that builds a shipping image rather
# than by a flag someone has to remember to type.
#
# Directory-scope `add_compile_definitions` rather than a per-target one: the
# guard sits in a first-party library source, and this file is included before
# any target is created on both paths that compile those sources (see the
# include site in cmake/ra8_shared_libs.cmake).
#

option(RA8_PRODUCTION_BUILD "Declare this image a shipping build (arms ship-only guards)" OFF)

if(RA8_PRODUCTION_BUILD)
  add_compile_definitions(RA8_PRODUCTION_BUILD)
endif()

#
# libs/ra_batt -- bounded, allocation-free low-battery nag policy.
#
# Edge-triggered low / critical battery warnings with hysteresis and a
# charging re-arm, decided from a stream of state-of-charge readings. Pure
# logic: no font, no framebuffer, no MMIO -- the display / UART / e-reader
# chrome layer on top. Built on ra_core (ra_err / ra_check) only.
#
# Layout:
#   inc/ra_batt.h   public API (nag levels, thresholds, monitor, update)
#   src/ra_batt.c   edge + hysteresis policy
#
# Consumers reach into inc/ + src/ directly: firmware apps list `ra_batt`
# in ra_add_app(LIBS ...); the host test build folds libs/ra_batt/src/*.c
# into the ra_core_hal object library (see tests/CMakeLists.txt).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Header-and-source library; no standalone build target. See the note above.

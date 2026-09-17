# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# libs/ra8_batt -- bounded, allocation-free low-battery nag policy.
#
# Edge-triggered low / critical battery warnings with hysteresis and a
# charging re-arm, decided from a stream of state-of-charge readings. Pure
# logic: no font, no framebuffer, no MMIO -- the display / UART / e-reader
# chrome layer on top. Built on ra8_core (ra8_err / ra8_check) only.
#
# Layout:
#   inc/ra8_batt.h   public API (nag levels, thresholds, monitor, update)
#   src/ra8_batt.c   edge + hysteresis policy
#
# Consumers reach into inc/ + src/ directly: firmware apps list `ra8_batt`
# in ra8_add_app(LIBS ...); the host test build folds libs/ra8_batt/src/*.c
# into the ra8_core_hal object library (see tests/CMakeLists.txt).
#
#

# Header-and-source library; no standalone build target. See the note above.

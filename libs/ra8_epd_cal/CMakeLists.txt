#
# libs/ra8_epd_cal -- per-device e-paper panel calibration (VCOM).
#
# Owns the on-flash calibration record (magic + schema version + CRC-32)
# and the boot-time resolution policy that decides which VCOM, if any, is
# trustworthy enough to drive the panel with. Includes no HAL header: the
# controller and the non-volatile store are reached through injected
# function-pointer seams, so every branch is host-testable.
#
# The record deliberately lives outside both DFU code banks (extra-MRAM,
# k_ra8_flash_extra_start + k_ra8_epd_cal_extra_mram_offset) so a firmware
# update or a rollback cannot erase per-unit calibration, and carries its
# own integrity check because per-device bytes cannot be covered by the
# Root-of-Trust image signature. See libs/ra8_epd_cal/inc/ra8_epd_cal.h.
#
# Each consumer (app or test) globs libs/ra8_epd_cal/src/*.c directly, the
# same way ra8_display_pal / ra8_ota / ra8_net_pal are wired in. This file
# is intentionally a no-op so the existing per-app CMakeLists.txt +
# tests/CMakeLists.txt patterns keep working.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Intentionally empty -- consumers reach into inc/ + src/ directly.

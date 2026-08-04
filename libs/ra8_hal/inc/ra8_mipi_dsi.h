/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_mipi_dsi.h
 * @brief MIPI DSI-2 host driver -- public API (full HUM Ch 65 surface)
 * @ingroup grp_hal_display
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Hand-written HAL wrapper around the RA8D2 MIPI DSI Host module
 * (HUM Ch 65, p 3839-3934). Pairs with the GLCDC (HUM Ch 63) on the
 * pixel-data side and the on-die D-PHY (HUM Ch 64) on the link side.
 *
 * ## Pairing with `ra8_mipi_phy`
 *
 * The D-PHY analog timing block lives in HUM Ch 64 and is owned by
 * a separate `ra8_mipi_phy` driver. This module **does not** touch the
 * PHY register window at `0x40346C00` -- the caller is expected to
 * bring the PHY up first (PLL locked, lanes out of LP-11) and then
 * call `ra8_mipi_dsi_init()` to configure the link/protocol layer on
 * top of it. Tearing down goes the other way: stop the DSI link, then
 * power down the PHY.
 *
 * ## Surface (this revision)
 *
 *  - Lifecycle: `_init / _deinit / _enter_stop / _exit_stop`.
 *  - HS clock control: `_hs_clock_start / _hs_clock_stop`.
 *  - Software reset helper: `_soft_reset`.
 *  - Sequence channel commands: `_send_short_packet`,
 *    `_send_long_packet`, `_send_command` (full BTA + LP/HS + aux op).
 *  - Read transactions: `_read_packet` (assembles BTA-then-read on
 *    sequence channel 0).
 *  - Video mode: `_video_configure`, `_video_start`, `_video_stop`,
 *    `_video_status_get`.
 *  - ULPS: `_ulps_enter`, `_ulps_exit` -- per lane (clock / data).
 *  - Receive path: `_rx_result_get`, `_rx_payload_read`,
 *    `_ack_error_get`, `_ack_error_clear`.
 *  - Status / IRQ: every sub-status register has a `_get_*` and
 *    `_clear_*` helper, every IER has a `_irq_enable_*` helper.
 *  - Tearing-effect: `_te_event_pending`, `_te_event_clear`.
 *  - Per-IRQ-source dispatch: `_dispatch_seq0`, `_dispatch_seq1`,
 *    `_dispatch_video`, `_dispatch_receive`, `_dispatch_fatal`,
 *    `_dispatch_phy` plus the legacy fanout `_dispatch`.
 *
 * ## Thin umbrella
 *
 * This header is a thin umbrella: the public surface is split across
 * `ra8_mipi_dsi_types.h` (enums / structs / typedefs) and
 * `ra8_mipi_dsi_api.h` (function prototypes). Consumers keep including
 * `ra8_mipi_dsi.h` unchanged; everything is re-exported below.
 */

#pragma once

#include "ra8_mipi_dsi_api.h"
#include "ra8_mipi_dsi_types.h"

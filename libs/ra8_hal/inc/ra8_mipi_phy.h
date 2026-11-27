/**
 * @file ra8_mipi_phy.h
 * @brief MIPI D-PHY driver -- physical layer shared by DSI and CSI
 * @ingroup grp_hal_display
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8D2 MIPI PHY block (HUM Ch 64, p 3822-3838).
 * The PHY is the physical layer underneath both the MIPI DSI host
 * (Ch 65) and the MIPI CSI receiver (Ch 66). Either client may own
 * the PHY at a given time; the driver explicitly tracks the active
 * mode so the PHY can be reprogrammed between DSI and CSI use cases
 * (see ``ra8_mipi_phy_switch_mode``).
 *
 * The full surface in this revision covers every documented
 * register field and every documented operating mode:
 *
 * - **Lifecycle** -- ``init``, ``deinit``, ``reset``, ``enter_stop``,
 * ``exit_stop``, ``recover_from_error``.
 * - **Mode control** -- ``switch_mode`` (DSI<->CSI without full
 * re-init), continuous-clock mode, EoTP enable/disable.
 * - **Lane configuration** -- ``set_lane_count`` (1/2 lanes), per-lane
 * enable/disable, ``set_lane_speed`` (PLL re-tune).
 * - **PLL / power** -- ``pll_start``, ``pll_stop``, ``ldo_enable``,
 * ``ldo_disable``, ``set_escape_divisor``, ``set_pclka_freq``.
 * - **Timing tables** -- ``select_timing`` looks up Tables 64.2 / 64.3
 * from HUM p 3831-3836 by (mode, PCLKA, lane rate) and applies the
 * corresponding ``ra8_mipi_phy_timing_t`` snapshot.
 * - **Status & IRQ** -- ``get_status``, ``clear_status``,
 * ``attach_handler`` / ``dispatch``, ``is_pll_locked``,
 * ``is_ldo_stable``, ``wait_ready``.
 *
 * # Module-stop note
 *
 * HUM Ch 11.2.8 Table 11.13 pp 446-447 and Ch 64.4.2 p 3838:
 * The RA8D2 shared D-PHY does not possess an independent MSTPCRC bit;
 * module-stop gating is tied to the parent subsystem:
 * - MSTPC10 (bit 10) for MIPI DSI Host mode (k_ra8_mstp_mipi_dsi)
 * - MSTPC17 (bit 17) for MIPI CSI-2 Device mode (k_ra8_mstp_mipi_csi)
 * (Note: Bit 13 in MSTPCRC is MSTPC13: DOC / Data Operation Circuit).
 * ``ra8_mipi_phy_init`` ungates the respective bit for the active mode.
 *
 * # Header layout
 *
 * This file is a thin umbrella. The declarations live in three
 * self-contained sub-headers, all in this directory:
 *
 * - ``ra8_mipi_phy_types.h`` -- typed enums, register-mirror structs,
 * and the status-callback typedef.
 * - ``ra8_mipi_phy_api.h`` -- lifecycle, status, interrupt-path, and
 * power-transition prototypes.
 * - ``ra8_mipi_phy_ops.h`` -- runtime operations prototypes.
 *
 * Consumers continue to ``#include "ra8_mipi_phy.h"`` unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_mipi_phy_api.h"
#include "ra8_mipi_phy_ops.h"
#include "ra8_mipi_phy_types.h"

#ifdef __cplusplus
}
#endif

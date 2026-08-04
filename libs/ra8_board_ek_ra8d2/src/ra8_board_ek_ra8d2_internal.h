/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_board_ek_ra8d2_internal.h
 * @brief Cross-translation-unit seam for the EK-RA8D2 BSP implementation
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * The EK-RA8D2 board-support implementation is split across several
 * translation units (``ra8_board_ek_ra8d2*.c``) that share one library.
 * This header carries the only declarations that must be visible to
 * more than one of those units: the MIPI/MIPI-DSI panel PLL constant
 * block, which is authored alongside the Octo-SPI bring-up in the
 * primary unit but consumed by the MIPI panel bring-up in a sibling
 * unit. Keeping it here keeps a single source-of-truth and avoids an
 * "undeclared identifier" break if either unit moves.
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

/**
 * @enum panel_pll_const_t
 * @brief MIPI panel PLL bring-up constants (pending datasheet confirm).
 *
 * @details
 * These two coefficients seed the D-PHY PLL solve in the MIPI panel
 * bring-up path. ``k_panel_pclka_mhz`` assumes the chip's CGC reset
 * default and ``k_panel_pll_nmul`` is the integer multiplier the
 * placeholder 480 Mbps/lane target lands on. They live in this shared
 * header because they are authored next to the Octo-SPI reset timings
 * in the primary unit yet referenced from the MIPI panel config in a
 * sibling unit.
 *
 * @invariant Values are placeholders until the panel datasheet is
 *            confirmed; both are interim self-consistent constants.
 * @see s_mipi_phy_cfg
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_panel_pclka_mhz = 60U, /**< PCLKA assumed CGC reset default. */
  k_panel_pll_nmul  = 48U, /**< PLL integer multiplier.          */
} panel_pll_const_t;

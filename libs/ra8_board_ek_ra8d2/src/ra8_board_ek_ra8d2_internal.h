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
 * unit, and the console readiness predicate the stream binding needs.
 * Keeping them here keeps a single source-of-truth and avoids an
 * "undeclared identifier" break if either unit moves.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"

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

/**
 * @brief Report whether ``ra8_board_uart_console_init`` has succeeded.
 *
 * @details
 * The console's "is it up" flag is private to the comms translation unit --
 * the write / read / flush entry points there consult it directly. The stream
 * binding in a sibling unit needs the same answer before it hands an
 * ::ra8_io_stream_t to an application, so the flag is published as a predicate
 * rather than as a mutable extern: a caller can ask, and cannot lie about it.
 *
 * Library-private (::RA8_PRIV): production code outside
 * ``libs/ra8_board_ek_ra8d2`` must use the public console API, which enforces
 * the same state itself.
 *
 * @return bool Console readiness.
 * @retval true  ``ra8_board_uart_console_init`` returned ::k_ra8_ok at least
 *               once, so SCI8 is configured and PD02 / PD03 are routed.
 * @retval false The console has never come up; nothing may be written to it.
 *
 * @pre The BSP comms translation unit is linked into the image.
 * @pre Called from a single-threaded application or boot context.
 * @post No board, pin or peripheral state is modified.
 * @post The returned value reflects the flag at the moment of the call.
 *
 * @note Not thread-safe against a concurrent ``ra8_board_uart_console_init``.
 *
 * @see ra8_board_uart_console_init  The call that sets the flag.
 *
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_ra8_board_uart_console_is_up(void);

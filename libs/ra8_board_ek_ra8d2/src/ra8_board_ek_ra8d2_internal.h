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
#include "ra8_err.h"

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

/**
 * @brief Bring up the Ethernet clock, power, MSTP, and COMA boundary.
 *
 * @details
 * Initializes ESWCLK and ESWPHYCLK, powers the ESWM domain, and publishes the
 * live ESWCLK frequency. It then acquires the ESWM module-stop reference and
 * runs the COMA reset, buffer-pool readiness poll, and agent clock fan-out.
 * These steps implement the prerequisites described by HUM Ch 9.10.23
 * "EtherSW Clock (ESWCLK)" p 405 and Ch 31 "Ethernet Common Agent (COMA)"
 * p 1590.
 *
 * @param[out] out_eswclk_hz Published ESWCLK frequency on success.
 *
 * @return The first CGC, MSTP, or COMA result.
 * @retval k_ra8_ok Clock, power, module-stop, and COMA bring-up completed.
 * @retval k_ra8_err_null_ptr @p out_eswclk_hz is nullptr.
 * @retval k_ra8_err_invalid_state An Ethernet module-stop reference count is saturated.
 * @retval k_ra8_err_hw_timeout A clock, module-stop, or COMA readiness poll timed out.
 *
 * @pre Fake MMIO and module-stop state are initialized.
 * @pre @p out_eswclk_hz points to writable storage.
 * @post Success publishes the configured ESWCLK frequency.
 * @post Failure stops before later Ethernet stages.
 * @note Library-private; host tests call this production boundary directly to
 *       exercise sequential error propagation.
 * @since 0.1.0
 */
RA8_PRIV RA8_NODISCARD ra8_err_t priv_ra8_board_eth_eswm_bring_up(uint32_t* out_eswclk_hz);

/**
 * @brief Walk the board Ethernet ETHA from RESET to CONFIG.
 *
 * @details
 * Initializes ETHA1 in RESET with all error interrupts masked, then requests
 * the ordered RESET-to-DISABLE-to-CONFIG transitions. Each transition polls
 * EAMS until it reflects the EAMC request, as specified by HUM Ch 32.3.1.1
 * "EAMC : Mode Configuration Register" p 1630 and Ch 32.3.1.2 "EAMS : Mode
 * Status Register" p 1631.
 *
 * @return The first ETHA initialization or mode-transition result.
 * @retval k_ra8_ok ETHA1 reached CONFIG mode.
 * @retval k_ra8_err_invalid_state The ESWM module-stop reference count is saturated.
 * @retval k_ra8_err_hw_timeout The ESWM release or an ETHA mode transition timed out.
 *
 * @pre Fake MMIO and module-stop state are initialized.
 * @pre The ESWM module-stop reference may be acquired by this call.
 * @post Success leaves ETHA1 in CONFIG.
 * @post Failure is propagated before later board initialization.
 * @note Library-private; host tests call this production boundary directly to
 *       exercise sequential error propagation.
 * @since 0.1.0
 */
RA8_PRIV RA8_NODISCARD ra8_err_t priv_ra8_board_eth_etha_to_config(void);

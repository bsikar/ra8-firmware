/**
 * @file ra8_mipi_phy_internal.h
 * @brief Module-private link seam shared between the MIPI D-PHY driver
 * @ingroup grp_hal_display
 *        translation units.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The MIPI D-PHY driver is implemented across more than one translation
 * unit so that no single file exceeds the
 * ``scripts/checks/check_file_size.py`` cap. The lifecycle / power / mode /
 * lane / status / IRQ path lives in ``ra8_mipi_phy.c``; the bulky HUM
 * Tables 64.2 (DSI) / 64.3 (CSI) timing matrix, its lookup walker, the
 * PLL-frequency arithmetic helper, and the ``ra8_mipi_phy_select_timing``
 * surface live in ``ra8_mipi_phy_timing.c``. Two helper functions are
 * called across the two TUs and are declared here.
 *
 * This header is private to the MIPI D-PHY driver only -- no other module
 * includes it. It exports:
 *  - ``internal_mipi_phy_write_timing``: defined in ``ra8_mipi_phy.c`` and
 *    used by the table-driven ``ra8_mipi_phy_select_timing`` in
 *    ``ra8_mipi_phy_timing.c``;
 *  - ``internal_mipi_phy_compute_freq``: defined in
 *    ``ra8_mipi_phy_timing.c`` and used by ``ra8_mipi_phy_validate_pll_band``
 *    in ``ra8_mipi_phy.c``.
 *
 * Read-only constants (the log tag) and the file-scope mutable driver
 * state are intentionally NOT shared: each is confined to the single TU
 * that uses it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_mipi_phy.h"

/**
 * @brief Write the six DPHYTIMx registers from the timing block.
 *
 * @details
 * Defined in ``ra8_mipi_phy.c`` and shared with the table-driven
 * ``ra8_mipi_phy_select_timing`` in ``ra8_mipi_phy_timing.c``. Programs
 * DPHYTIM1..DPHYTIM6 (HUM Ch 64.2.8 - 64.2.13 p 3827-3830) from the
 * caller-supplied timing snapshot.
 *
 * @param[in] t Timing block whose fields are packed into DPHYTIM1..6.
 *
 * @pre ``t`` is a valid, non-NULL ``ra8_mipi_phy_timing_t``.
 * @pre The MIPI PHY module clock is ungated.
 * @post DPHYTIM1..DPHYTIM6 reflect the fields of ``*t``.
 * @post No register other than DPHYTIM1..6 is modified.
 *
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
RA8_PRIV void internal_mipi_phy_write_timing(const ra8_mipi_phy_timing_t* t);

/**
 * @brief Compute the PLL output frequency for a ``(mosc, pll)`` tuple.
 *
 * @details
 * Defined in ``ra8_mipi_phy_timing.c`` and shared with
 * ``ra8_mipi_phy_validate_pll_band`` in ``ra8_mipi_phy.c``. Implements the
 * HUM Ch 64.2.2 p 3823 relation ``f = fMAIN * I * (NF + N) * P`` using
 * hundredths fixed-point arithmetic.
 *
 * @param[in] pll      PLL coefficient block (IDIV / NFMUL / PMUL / NMUL).
 * @param[in] mosc_mhz Main-oscillator frequency in MHz.
 *
 * @return PLL output frequency in MHz, or 0 if the divisor product is 0.
 * @retval 0 The composed divisor product was zero.
 * @retval other Computed PLL output frequency in MHz.
 *
 * @pre ``pll`` is a valid, non-NULL ``ra8_mipi_phy_pll_t``.
 * @pre ``mosc_mhz`` is a non-zero main-oscillator frequency.
 * @post No register or shared state is modified by this helper.
 * @post The return value is a pure function of the two arguments.
 *
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
RA8_PRIV uint32_t internal_mipi_phy_compute_freq(const ra8_mipi_phy_pll_t* pll, uint8_t mosc_mhz);

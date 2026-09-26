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
 *  - ``priv_mipi_phy_write_timing``: defined in ``ra8_mipi_phy.c`` and
 *    used by the table-driven ``ra8_mipi_phy_select_timing`` in
 *    ``ra8_mipi_phy_timing.c``;
 *  - ``priv_mipi_phy_compute_freq``: defined in
 *    ``ra8_mipi_phy_timing.c`` and used by ``ra8_mipi_phy_validate_pll_band``
 *    in ``ra8_mipi_phy.c`` and by ``ra8_mipi_phy_compute_pll_freq`` in
 *    ``ra8_mipi_phy_ops.c``;
 *  - ``priv_mipi_phy_find_timing``: defined in ``ra8_mipi_phy_timing.c``,
 *    the register-free half of ``ra8_mipi_phy_select_timing``, shared with
 *    ``ra8_mipi_phy_lookup_timing`` in ``ra8_mipi_phy_ops.c``;
 *  - ``k_ra8_mipi_phy_mstpc_bit``: the provisional module-stop slot, written
 *    by ``ra8_mipi_phy.c`` and read by the lifecycle observers in
 *    ``ra8_mipi_phy_ops.c``.
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
RA8_PRIV void priv_mipi_phy_write_timing(const ra8_mipi_phy_timing_t* t);

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
RA8_PRIV uint32_t priv_mipi_phy_compute_freq(const ra8_mipi_phy_pll_t* pll, uint8_t mosc_mhz);

/**
 * @enum ra8_mipi_phy_mstpc_bit_t
 * @brief Direct-write fallback for the MIPI PHY module-stop bit.
 *
 * @details
 * The shared ``ra8_mstp_t`` enum in ``libs/ra8_hal/inc/ra8_mstp_regs.h``
 * does NOT yet have a ``k_ra8_mipi_phy`` entry, and this driver must not
 * extend that file. As a stop-gap the driver clears MSTPCRC bit 13 (the
 * MIPI PHY slot in MSTPCRC -- HUM Ch 64.4.2 p 3838 references MSTPCRC for
 * the block) directly, and the observers in ``ra8_mipi_phy_ops.c`` read the
 * same bit to tell "module stopped" from "idle".
 *
 * TODO: When ``ra8_mstp_regs.h`` gains an explicit ``k_ra8_mstp_mipi_phy``
 * value (driven by HUM Ch 11.2.8 "MSTPCRC" p 446-447), replace the direct
 * register accesses with ``ra8_mstp_enable(k_ra8_mstp_mipi_phy)``.
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_mstpc_bit = 13U, /**< Provisional MSTPC slot. */
} ra8_mipi_phy_mstpc_bit_t;

/**
 * @brief Find the HUM Table 64.2 / 64.3 row for a ``(mode, pclka, rate)``.
 *
 * @details
 * Defined in ``ra8_mipi_phy_timing.c``. The argument validation and the
 * linear table scan of ``ra8_mipi_phy_select_timing`` without the
 * DPHYTIM1..6 write, so that the public dry-run entry point
 * ``ra8_mipi_phy_lookup_timing`` (``ra8_mipi_phy_ops.c``) and the
 * programming entry point share one matcher.
 *
 * @param[in]  mode       Active mode; DSI and CSI use different tables.
 * @param[in]  pclka_mhz  PCLKA frequency, MHz.
 * @param[in]  rate_mbps  Per-lane line rate, Mbps.
 * @param[out] out_timing Non-NULL destination for the matching row.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Row found, ``*out_timing`` filled.
 * @retval k_ra8_err_invalid_arg     ``mode`` outside the enum or
 *                                   ``rate_mbps`` outside 80..720.
 * @retval k_ra8_err_not_supported   No row matches ``pclka_mhz``.
 *
 * @pre ``out_timing`` is a valid, non-NULL ``ra8_mipi_phy_timing_t``.
 * @post ``*out_timing`` is written only on ``k_ra8_ok``.
 * @post No register is modified.
 *
 * @note Internal helper. Pure lookup over a static table.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mipi_phy_find_timing(ra8_mipi_phy_mode_t    mode,
                                             uint8_t                pclka_mhz,
                                             uint16_t               rate_mbps,
                                             ra8_mipi_phy_timing_t* out_timing);

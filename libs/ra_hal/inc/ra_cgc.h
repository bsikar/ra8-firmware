/**
 * @file ra_cgc.h
 * @brief High-level Clock Generation Circuit driver
 *
 * @details
 * Minimal clock-init surface for bring-up. The RA8D2 defaults to MOCO
 * (~8 MHz) out of reset, which is enough to blink an LED and exercise
 * the IOPORT block without touching CGC at all. Use the helpers here
 * to move up to HOCO (20 MHz) or the full PLL1 path (CPUCLK0 up to
 * 1 GHz) as the firmware grows.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra_err.h"

/**
 * @brief Configure the clock tree to a safe default.
 *
 * @details
 * For v0.x bring-up this function is intentionally a no-op: the
 * firmware runs on MOCO (8 MHz) until a real PLL bring-up lands.
 * Returning `k_ra_ok` lets `main()` keep the pattern:
 *
 * @code{.c}
 *   ra_infrastructure_init();
 *   RA_ERROR_CHECK(ra_cgc_init());
 *   // ... drivers ...
 * @endcode
 *
 * which will automatically start using the real implementation once
 * this function is filled out.
 *
 * @return Always `k_ra_ok` today. Will return clock-setup errors once
 *         a real PLL routine is wired in.
 */
[[nodiscard]] ra_err_t ra_cgc_init(void);

/**
 * @brief Switch the system clock source to HOCO (20 MHz).
 *
 * @details
 * Starts the HOCO if it is stopped, waits for it to stabilise, then
 * writes `k_ra_cksel_hoco` to SCKSCR. Requires a PRCR unlock around
 * the SCKSCR write.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok                Clock switched.
 * @retval k_ra_err_hw_timeout    HOCO failed to report ready within the
 *                                 timeout window.
 */
[[nodiscard]] ra_err_t ra_cgc_use_hoco(void);

/**
 * @enum ra_clock_id_t
 * @brief Identifiers for the clock-tree frequencies queryable at runtime.
 */
typedef enum : uint8_t {
  k_ra_clock_id_cpuclk0 = 0U, /**< Cortex-M85 CPUCLK0.         */
  k_ra_clock_id_cpuclk1 = 1U, /**< Cortex-M33 CPUCLK1.         */
  k_ra_clock_id_iclk    = 2U, /**< System ICLK.                */
  k_ra_clock_id_pclka   = 3U, /**< PCLKA.                      */
  k_ra_clock_id_pclkb   = 4U, /**< PCLKB.                      */
  k_ra_clock_id_pclkc   = 5U, /**< PCLKC.                      */
  k_ra_clock_id_pclkd   = 6U, /**< PCLKD.                      */
  k_ra_clock_id_pclke   = 7U, /**< PCLKE.                      */
  k_ra_clock_id_fclk    = 8U, /**< Flash/MRAM interface clock. */
  k_ra_clock_id_mriclk  = 9U, /**< MRAM bus clock.             */
} ra_clock_id_t;

/**
 * @brief Query the current frequency of a clock-tree domain.
 *
 * @details
 * Returns the value last programmed by `ra_cgc_init()` (or the
 * reset default MOCO value if `ra_cgc_init()` has not yet run).
 * Drivers that need to compute baud rates or sampling periods
 * should always go through this function rather than hard-coding
 * values from `ra_time_constants.h`.
 *
 * @param[in]  id      Clock identifier.
 * @param[out] out_hz  On success, current frequency in Hz.
 * @return `k_ra_ok` on success, `k_ra_err_invalid_arg` if `out_hz`
 *         is NULL or `id` is out of range.
 */
[[nodiscard]] ra_err_t ra_cgc_get_clock_hz(ra_clock_id_t id, uint32_t* out_hz);

#ifdef __cplusplus
}
#endif

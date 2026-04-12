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

#ifdef __cplusplus
}
#endif

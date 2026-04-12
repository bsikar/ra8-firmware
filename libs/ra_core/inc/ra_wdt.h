/**
 * @file ra_wdt.h
 * @brief Software Watchdog Timer driver header
 *
 * @details
 * Thin wrapper around `ra8d2_wdt_regs.h`. The WDT is the
 * software-clocked companion to the IWDT. Where the IWDT runs from
 * its own oscillator and cannot be stopped, the WDT shares PCLKB and
 * can be halted by the debugger -- useful for application liveness
 * checks that should pause while you are stepping.
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
 * @brief Initialise the software WDT.
 *
 * @return `k_ra_ok` -- stub today; extend once a policy exists.
 */
[[nodiscard]] ra_err_t ra_wdt_init(void);

/**
 * @brief Refresh the software WDT counter.
 */
void ra_wdt_refresh_deferred(void);

#ifdef __cplusplus
}
#endif

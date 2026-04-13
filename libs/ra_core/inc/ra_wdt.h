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

#include <stdint.h>

#include "ra_err.h"

/**
 * @enum ra_wdt_status_mask_t
 * @brief WDTSR status bits.
 */
typedef enum : uint16_t {
  k_ra_wdt_status_none      = 0x0000U,
  k_ra_wdt_status_underflow = 0x4000U, /**< WDTSR.UNDFF bit14. */
  k_ra_wdt_status_refresh   = 0x8000U, /**< WDTSR.REFEF  bit15. */
} ra_wdt_status_mask_t;

/**
 * @typedef ra_wdt_event_fn_t
 * @brief WDT event callback.
 */
typedef void (*ra_wdt_event_fn_t)(void* ctx, uint16_t status_mask);

/**
 * @brief Initialise the software WDT.
 * @return `k_ra_ok` -- stub today; extend once a policy exists.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_wdt_init(void);

/**
 * @brief Refresh the software WDT counter.
 */
void ra_wdt_refresh_deferred(void);

/**
 * @brief Read the WDTSR status bits.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_wdt_get_status(uint16_t* out_mask);

/**
 * @brief Clear WDTSR status flags.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_wdt_clear_status(void);

/**
 * @brief Attach a callback for the WDT event.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_wdt_attach_handler(ra_wdt_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch a WDT event -- snapshot + fire callback.
 * @since 0.2.0
 */
void ra_wdt_dispatch(void);

#ifdef __cplusplus
}
#endif

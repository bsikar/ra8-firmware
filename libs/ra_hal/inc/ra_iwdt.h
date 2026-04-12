/**
 * @file ra_iwdt.h
 * @brief Independent Watchdog driver header
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
 * @brief Initialise the IWDT driver layer.
 *
 * @return `k_ra_ok` -- stub; OFS0 controls the actual period.
 */
[[nodiscard]] ra_err_t ra_iwdt_init(void);

/**
 * @brief Refresh the IWDT counter via a wrapper function.
 *
 * @details
 * Extern wrapper around the inline `ra_iwdt_refresh()` helper in
 * `ra8d2_iwdt_regs.h` so other translation units can refresh the
 * IWDT without pulling in the register header.
 */
void ra_iwdt_refresh_deferred(void);

#ifdef __cplusplus
}
#endif

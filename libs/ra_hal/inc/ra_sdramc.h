/**
 * @file ra_sdramc.h
 * @brief External SDRAM controller driver (framework)
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
 * @brief Initialise the external SDRAM at `k_ra_sdram_base_addr`.
 *
 * @details
 * Runs the documented SDRAM bring-up sequence (precharge all ->
 * mode register write -> refresh enable). Uses EK-RA8D2 board
 * defaults; change the refresh interval for a different panel.
 */
[[nodiscard]] ra_err_t ra_sdramc_init(void);

#ifdef __cplusplus
}
#endif

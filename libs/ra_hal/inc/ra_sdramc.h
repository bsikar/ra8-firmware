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
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sdramc_init(void);

/**
 * @brief Tear down the SDRAM controller (disable refresh, clear SDICR).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sdramc_deinit(void);

/**
 * @brief Change the SDRAM auto-refresh interval at runtime.
 * @param[in] sdrfcr New SDRFCR value.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sdramc_set_refresh_interval(uint16_t sdrfcr);

/**
 * @brief Read the refresh-enable bit as a status mask.
 * @param[out] out_enabled Non-zero when SDRFEN is set.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sdramc_get_status(uint8_t* out_enabled);

/**
 * @brief Disable auto-refresh for deep-sleep entry.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sdramc_enter_stop(void);

/**
 * @brief Re-enable auto-refresh after wake-up.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sdramc_exit_stop(void);

#ifdef __cplusplus
}
#endif

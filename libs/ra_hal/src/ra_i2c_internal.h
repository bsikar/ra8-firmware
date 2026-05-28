/**
 * @file ra_i2c_internal.h
 * @brief Test-access surface for ra_i2c internal helpers (MC/DC).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief Pure predicate: either clock argument is zero.
 *
 * @details
 * Promoted from the OR decision in ``internal_i2c_bitrate`` (also used
 * by ``ra_i2c_init``) so the two conditions can be exercised with
 * independent influence under MC/DC.
 *
 * @param[in] bus_hz   Target bus clock in Hz.
 * @param[in] pclkb_hz Reference PCLKB clock in Hz.
 *
 * @return Boolean reject predicate.
 * @retval true  At least one clock is zero (invalid).
 * @retval false Both clocks are non-zero.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the two inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition OR; N+1 = 3 vectors:
 *  - V1: bus!=0, pclkb!=0 -> false
 *  - V2: bus=0,  pclkb!=0 -> true  (varies left)
 *  - V3: bus!=0, pclkb=0  -> true  (varies right)
 *
 * @since 0.1.0
 */
bool ra_i2c_internal_clk_invalid(uint32_t bus_hz, uint32_t pclkb_hz);

#ifdef __cplusplus
}
#endif

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

/**
 * @brief Pure predicate: ``index`` is the second-to-last byte of ``len``.
 *
 * @details
 * Promoted from the AND decision in ``internal_i2c_rx_ack_phase`` (the
 * WAIT-arm gate on the second-to-last receive byte) so the two
 * conditions can be exercised with independent influence under MC/DC.
 *
 * @param[in] index Zero-based byte index about to be read.
 * @param[in] len   Total number of bytes to read.
 *
 * @return Boolean WAIT-arm predicate.
 * @retval true  ``index`` is the second-to-last byte (arm WAIT).
 * @retval false Otherwise.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the two inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition AND; N+1 = 3 vectors:
 *  - V1: len>=2, index==len-2 -> true
 *  - V2: len<2  (index=0,len=1) -> false (varies left, short-circuits)
 *  - V3: len>=2, index!=len-2   -> false (varies right)
 *
 * @since 0.1.0
 */
bool ra_i2c_internal_is_wait_byte(uint32_t index, uint32_t len);

#ifdef __cplusplus
}
#endif

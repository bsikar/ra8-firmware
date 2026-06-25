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

#include "ra8d2_i2c_regs.h"

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
 * @var s_i2c_tag
 * @brief Log tag shared by the I2C transfer and configuration planes.
 *
 * @details
 * Defined once in ``ra_i2c.c`` (the data-transfer translation unit) and
 * consumed by both ``ra_i2c.c`` and ``ra_i2c_config.c`` so the two halves
 * of the split driver log under the same "I2C" tag.
 *
 * @note Read-only string pointer; not mutated after static init.
 *
 * @since 0.1.0
 */
extern const char* const s_i2c_tag;

/**
 * @struct ra_i2c_state_t
 * @brief Per-channel driver state shared by both I2C translation units.
 *
 * @details
 * Tracks initialization and bus-held ownership for each RIIC channel. The
 * data-transfer plane sets ``bus_held`` after a ``send_stop = false`` write
 * and the configuration plane clears both fields on ``ra_i2c_init`` /
 * ``ra_i2c_deinit``.
 *
 * @invariant ``bus_held`` is only true between a held write and its chained
 *            transfer; cleared on every STOP path and on (de)init.
 *
 * @see s_i2c_state
 *
 * @since 0.1.0
 */
typedef struct {
  bool initialized; /**< Tracks ``ra_i2c_init`` / ``ra_i2c_deinit``.       */
  bool bus_held;    /**< True when the previous write returned with
                       ``send_stop=false`` so the next call must inject a
                       repeated-START instead of a fresh START.           */
} ra_i2c_state_t;

/**
 * @var s_i2c_state
 * @brief Per-channel state table indexed by channel.
 *
 * @details
 * Defined once in ``ra_i2c.c`` and shared with ``ra_i2c_config.c`` so the
 * bring-up and transfer planes observe the same bus-ownership state.
 *
 * @warning Mutated only by the driver under the not-thread-safe contract.
 *
 * @see ra_i2c_state_t
 *
 * @since 0.1.0
 */
extern ra_i2c_state_t s_i2c_state[k_ra_i2c_channel_count];

#ifdef __cplusplus
}
#endif

/**
 * @file ra_dac8.h
 * @brief 8-bit D/A Converter (DAC8) driver header
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public API for an FSP-shaped 8-bit DAC peripheral driver. The
 * RA8D2 ships only the 12-bit DAC_B variant; this driver exposes the
 * FSP ``r_dac8`` open / write / start / stop / close surface so
 * example projects targeting smaller RA SKUs build unchanged.
 *
 * @warning RA8D2 silicon may not include this peripheral; verify
 *          against the BSP feature header before flashing. State is
 *          kept in a tiny static table -- no register writes are
 *          performed.
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
 * @enum ra_dac8_oper_mode_t
 * @brief Operating-mode selector (mirrors FSP ``dac8_mode_t``).
 */
typedef enum : uint8_t {
  k_ra_dac8_mode_normal   = 0U, /**< Normal direct-write mode. */
  k_ra_dac8_mode_realtime = 1U, /**< Real-time event-driven mode. */
} ra_dac8_oper_mode_t;

/**
 * @struct ra_dac8_cfg_t
 * @brief Open-time configuration descriptor.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t             channel;     /**< Channel index 0..1.                */
  ra_dac8_oper_mode_t mode;        /**< Operation mode.                    */
  bool                charge_pump; /**< True -> enable charge pump.        */
  bool                output_amp;  /**< True -> enable output amplifier.   */
} ra_dac8_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open one DAC8 channel.
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra_err_t`` error code.
 * @pre IRQs masked.
 * @pre ``cfg->channel`` is 0 or 1.
 * @post Channel state is open.
 * @post DAC value resets to zero.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dac8_open(const ra_dac8_cfg_t* cfg);

/**
 * @brief Close one DAC8 channel.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dac8_close(uint8_t channel);

/**
 * @brief Write an 8-bit code to ``channel``.
 * @param[in] channel Channel 0..1.
 * @param[in] value 8-bit DAC code.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dac8_write(uint8_t channel, uint8_t value);

/**
 * @brief Assert DACEN on ``channel``.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dac8_start(uint8_t channel);

/**
 * @brief Clear DACEN on ``channel``.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dac8_stop(uint8_t channel);

/**
 * @brief Read packed status mask.
 * @param[in] channel Channel 0..1.
 * @param[out] out_mask Bit 0 = open, bit 1 = running, bit 2 = pump, bit 3 = amp.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dac8_get_status(uint8_t channel, uint8_t* out_mask);

#ifdef __cplusplus
}
#endif

/**
 * @file ra_dac.h
 * @brief Legacy 12-bit D/A Converter (DAC12) driver header
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public API for an FSP-shaped DAC12 peripheral driver. The legacy
 * pre-RA8 chips expose a two-channel 12-bit DAC at this register
 * shape; the RA8D2 has the new DAC_B variant (see ``ra_dac_b.c``).
 * This driver exposes the FSP ``r_dac`` open / write / start / stop
 * / close surface so example projects targeting other RA SKUs build
 * unchanged on RA8D2.
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
 * @enum ra_dac_data_format_t
 * @brief Data placement in DADR register (mirrors FSP ``dac_data_format_t``).
 */
typedef enum : uint8_t {
  k_ra_dac_format_right = 0U, /**< Right-justified 12-bit data. */
  k_ra_dac_format_left  = 1U, /**< Left-justified 12-bit data.  */
} ra_dac_data_format_t;

/**
 * @struct ra_dac_cfg_t
 * @brief Open-time configuration descriptor.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t              channel;     /**< Channel index 0..1.                  */
  ra_dac_data_format_t data_format; /**< Data placement.                       */
  bool                 ad_da_sync;  /**< True -> sync with ADC unit.           */
  bool                 output_amp;  /**< True -> enable output amplifier.      */
} ra_dac_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open one DAC12 channel.
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra_err_t`` error code.
 * @pre IRQs masked.
 * @pre ``cfg->channel`` is 0 or 1.
 * @post Channel is recorded as open with format/amp flags from ``cfg``.
 * @post DACEN is left clear until ``ra_dac_start`` is called.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dac_open(const ra_dac_cfg_t* cfg);

/**
 * @brief Close one DAC12 channel.
 * @param[in] channel Channel index 0..1.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dac_close(uint8_t channel);

/**
 * @brief Write a 12-bit code to ``channel``.
 * @param[in] channel Channel index 0..1.
 * @param[in] value Raw 12-bit code (clamped to 0..4095).
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dac_write(uint8_t channel, uint16_t value);

/**
 * @brief Assert DACEN on ``channel`` (FSP R_DAC_Start equivalent).
 * @param[in] channel Channel index 0..1.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dac_start(uint8_t channel);

/**
 * @brief Clear DACEN on ``channel`` (FSP R_DAC_Stop equivalent).
 * @param[in] channel Channel index 0..1.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dac_stop(uint8_t channel);

/**
 * @brief Read packed status mask for ``channel``.
 * @param[in] channel Channel index 0..1.
 * @param[out] out_mask Bit 0 = open, bit 1 = running, bit 2 = amp.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dac_get_status(uint8_t channel, uint8_t* out_mask);

#ifdef __cplusplus
}
#endif

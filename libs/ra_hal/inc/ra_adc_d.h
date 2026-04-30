/**
 * @file ra_adc_d.h
 * @brief Delta-Sigma A/D Converter (ADC-D) driver header
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public API for an FSP-shaped ADC-D peripheral driver. ADC-D is the
 * delta-sigma A/D variant on small RA chips. The RA8D2 instead
 * carries the ADC-B successive-approximation block (see
 * ``ra_adc.c``); this driver exposes the FSP ``r_adc_d`` open / scan
 * / read / status surface so example projects targeting other RA
 * SKUs build unchanged on RA8D2.
 *
 * @warning RA8D2 silicon may not include this peripheral; verify
 *          against the BSP feature header before flashing. Internal
 *          state is kept in a tiny static table -- no register
 *          writes are performed.
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
 * @enum ra_adc_d_resolution_t
 * @brief Output resolution selection (mirrors FSP ``adc_d_resolution_t``).
 */
typedef enum : uint8_t {
  k_ra_adc_d_resolution_16 = 0U, /**< 16-bit data path. */
  k_ra_adc_d_resolution_24 = 1U, /**< 24-bit data path. */
} ra_adc_d_resolution_t;

/**
 * @struct ra_adc_d_cfg_t
 * @brief Open-time configuration descriptor.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t               channel_mask; /**< 1 bit per channel (0..7). */
  ra_adc_d_resolution_t resolution;   /**< Output resolution.        */
  uint16_t              osr;          /**< Oversampling ratio.       */
  bool                  continuous;   /**< True -> free-run scan.    */
} ra_adc_d_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_adc_d_scan_fn_t
 * @brief Scan-complete callback.
 * @param[in] ctx Caller context.
 */
typedef void (*ra_adc_d_scan_fn_t)(void* ctx);

/**
 * @brief Open the ADC-D unit with a config descriptor.
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra_err_t`` error code.
 * @pre IRQs masked.
 * @pre ``cfg->channel_mask`` is non-zero.
 * @post Driver state is open.
 * @post Configured resolution / OSR is recorded.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_d_open(const ra_adc_d_cfg_t* cfg);

/**
 * @brief Close the ADC-D unit.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_d_close(void);

/**
 * @brief Trigger a single software-start scan.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_d_scan_start(void);

/**
 * @brief Read the most-recent sample for ``channel``.
 * @param[in] channel Channel index 0..7.
 * @param[out] out_sample 24-bit sample, sign-extended into int32.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_d_read(uint8_t channel, int32_t* out_sample);

/**
 * @brief Read packed status word.
 * @param[out] out_status Bit 0 = open, bit 1 = scan-complete.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_d_get_status(uint8_t* out_status);

/**
 * @brief Attach a scan-complete callback.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_d_attach_handler(ra_adc_d_scan_fn_t fn, void* ctx);

/**
 * @brief Dispatch a scan-complete event.
 * @since 0.1.0
 */
void ra_adc_d_dispatch(void);

#ifdef __cplusplus
}
#endif

/**
 * @file ra_dmac.h
 * @brief 8-channel DMA controller (DMAC) driver
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
 * @enum ra_dmac_width_t
 * @brief Transfer width for DMAC operations.
 */
typedef enum : uint8_t {
  k_ra_dmac_width_byte = 0U, /**< 8-bit transfers.  */
  k_ra_dmac_width_half = 1U, /**< 16-bit transfers. */
  k_ra_dmac_width_word = 2U, /**< 32-bit transfers. */
} ra_dmac_width_t;

/**
 * @struct ra_dmac_config_t
 * @brief DMAC channel configuration.
 */
typedef struct {
  uint32_t        src;     /**< Source address.                   */
  uint32_t        dst;     /**< Destination address.              */
  uint16_t        count;   /**< Transfer count (DMCRA).           */
  ra_dmac_width_t width;   /**< Transfer width.                   */
  bool            src_inc; /**< Increment source address.         */
  bool            dst_inc; /**< Increment destination address.    */
} ra_dmac_config_t;

/**
 * @brief Programme and enable a DMAC channel.
 */
[[nodiscard]] ra_err_t ra_dmac_start(uint8_t channel, const ra_dmac_config_t* cfg);

/**
 * @brief Disable a DMAC channel.
 */
[[nodiscard]] ra_err_t ra_dmac_stop(uint8_t channel);

#ifdef __cplusplus
}
#endif

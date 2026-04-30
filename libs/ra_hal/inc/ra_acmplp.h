/**
 * @file ra_acmplp.h
 * @brief Low-Power Analog Comparator (ACMPLP) driver header
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public API for an FSP-shaped ACMPLP peripheral driver. ACMPLP is
 * the low-power comparator block found on smaller RA SKUs; the
 * RA8D2 implements the high-speed ACMPHS variant instead. This
 * driver exposes the FSP ``r_acmplp`` open / close / output / status
 * surface so example projects targeting other RA chips can build
 * unchanged on RA8D2.
 *
 * @warning RA8D2 silicon may not include this peripheral; verify
 *          against the BSP feature header before flashing. The
 *          driver wraps a placeholder register block kept in static
 *          state so unit tests pass without real hardware.
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
 * @enum ra_acmplp_speed_t
 * @brief Operation-speed selector (mirrors FSP ``acmplp_speed_t``).
 */
typedef enum : uint8_t {
  k_ra_acmplp_speed_low  = 0U, /**< Low-speed / lowest current.   */
  k_ra_acmplp_speed_high = 1U, /**< High-speed / faster response. */
} ra_acmplp_speed_t;

/**
 * @struct ra_acmplp_cfg_t
 * @brief Open-time configuration descriptor.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t           channel;    /**< Channel index 0..1.                   */
  ra_acmplp_speed_t speed;      /**< Operating speed.                      */
  uint8_t           ivpsel;     /**< Plus input selector.                  */
  uint8_t           ivrefsel;   /**< Reference input selector.             */
  bool              filter_en;  /**< True -> enable digital filter.        */
  bool              invert_out; /**< True -> invert comparator output.     */
} ra_acmplp_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_acmplp_event_fn_t
 * @brief Comparator edge callback.
 * @param[in] ctx Caller context.
 * @param[in] channel Channel that fired.
 */
typedef void (*ra_acmplp_event_fn_t)(void* ctx, uint8_t channel);

/**
 * @brief Open one ACMPLP channel.
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra_err_t`` error code.
 * @pre IRQs masked.
 * @pre ``cfg->channel`` is in range.
 * @post Channel state is open.
 * @post Filter / invert flags reflect ``cfg``.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_acmplp_open(const ra_acmplp_cfg_t* cfg);

/**
 * @brief Close one ACMPLP channel.
 * @param[in] channel Channel index 0..1.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_acmplp_close(uint8_t channel);

/**
 * @brief Read live output of ``channel``.
 * @param[in] channel Channel index 0..1.
 * @param[out] out_high True if comparator output is high.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_acmplp_read(uint8_t channel, bool* out_high);

/**
 * @brief Read packed status mask.
 * @param[in] channel Channel index 0..1.
 * @param[out] out_mask Bit 0 = open, bit 1 = output, bit 2 = speed.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_acmplp_get_status(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Attach a global edge callback.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_acmplp_attach_handler(ra_acmplp_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an edge event.
 * @param[in] channel Channel that fired.
 * @since 0.1.0
 */
void ra_acmplp_dispatch(uint8_t channel);

#ifdef __cplusplus
}
#endif

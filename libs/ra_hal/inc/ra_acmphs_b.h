/**
 * @file ra_acmphs_b.h
 * @brief Analog Comparator High-Speed B (ACMPHS-B) driver header
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public API for an FSP-shaped ACMPHS-B peripheral driver. The
 * silicon variant ACMPHS-B is the RA-family revision-B comparator
 * block (Renesas FSP module ``r_acmphs_b``). The RA8D2 ships the
 * original ACMPHS instead -- this driver therefore wraps a
 * placeholder register block kept in driver state so example
 * projects from FSP can be ported byte-compatibly without
 * having to swap their open / close / status calls.
 *
 * @warning RA8D2 silicon may not include this peripheral; verify
 *          against the BSP feature header before flashing. The
 *          driver uses a pure-software placeholder so unit tests
 *          and host builds pass even when the IP is absent.
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
 * @enum ra_acmphs_b_edge_t
 * @brief Edge detection mode (mirrors FSP ``acmphs_b_edge_t``).
 */
typedef enum : uint8_t {
  k_ra_acmphs_b_edge_none = 0U, /**< No edge detect.       */
  k_ra_acmphs_b_edge_rise = 1U, /**< Rising edge.          */
  k_ra_acmphs_b_edge_fall = 2U, /**< Falling edge.         */
  k_ra_acmphs_b_edge_both = 3U, /**< Either edge.          */
} ra_acmphs_b_edge_t;

/**
 * @struct ra_acmphs_b_cfg_t
 * @brief Configuration descriptor passed to ``ra_acmphs_b_open``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t            channel;    /**< Logical channel index (0..3).        */
  uint8_t            ivpsel;     /**< Plus input selector.                 */
  uint8_t            ivrefsel;   /**< Reference input selector.            */
  ra_acmphs_b_edge_t edge;       /**< Edge detection mode.                 */
  bool               filter_en;  /**< True -> enable digital filter.       */
  bool               invert_out; /**< True -> invert comparator output.    */
} ra_acmphs_b_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_acmphs_b_event_fn_t
 * @brief Edge-event callback.
 * @param[in] ctx Caller context.
 * @param[in] channel Channel that fired.
 */
typedef void (*ra_acmphs_b_event_fn_t)(void* ctx, uint8_t channel);

/**
 * @brief Open one ACMPHS-B channel with a config descriptor.
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra_err_t`` error code.
 * @pre IRQs masked or single-threaded init context.
 * @pre ``cfg->channel`` is in range.
 * @post Channel state is recorded as open.
 * @post Edge / filter / invert flags reflect ``cfg``.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_acmphs_b_open(const ra_acmphs_b_cfg_t* cfg);

/**
 * @brief Close one ACMPHS-B channel.
 * @param[in] channel Channel index 0..3.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_acmphs_b_close(uint8_t channel);

/**
 * @brief Read the live comparator output for ``channel``.
 * @param[in] channel Channel index 0..3.
 * @param[out] out_high True if comparator output is high.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_acmphs_b_read(uint8_t channel, bool* out_high);

/**
 * @brief Read packed status mask for one channel.
 * @param[in] channel Channel index 0..3.
 * @param[out] out_mask Bit 0=open, bit 1=last output, bits[3:2]=edge.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_acmphs_b_get_status(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Attach a global edge callback (shared by all channels).
 * @param[in] fn Callback fired on dispatch.
 * @param[in] ctx Context forwarded to the callback.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_acmphs_b_attach_handler(ra_acmphs_b_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an edge event for ``channel``.
 * @param[in] channel Channel that fired.
 * @since 0.1.0
 */
void ra_acmphs_b_dispatch(uint8_t channel);

#ifdef __cplusplus
}
#endif

/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_acmphs.h
 * @brief Full-featured High-Speed Analog Comparator (ACMPHS) driver
 * @ingroup grp_hal_analog
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * full build-out of the RA8D2 ACMPHS block. Extends
 * the enable/read stub with: per-channel descriptor
 * init, deinit, runtime input selection, edge-sensitive IRQ
 * attach / dispatch, filter config, power transition.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_acmphs_regs.h"
#include "ra8_err.h"
#include "ra8_port_constants.h"

/**
 * @enum ra8_acmphs_edge_t
 * @brief Edge detection mode (CMPCTL.CEG).
 */
typedef enum : uint8_t {
  k_ra8_acmphs_edge_none = 0U, /**< Polling only, no edge trigger. */
  k_ra8_acmphs_edge_rise = 1U, /**< Rising edge.                   */
  k_ra8_acmphs_edge_fall = 2U, /**< Falling edge.                  */
  k_ra8_acmphs_edge_both = 3U, /**< Either edge.                   */
} ra8_acmphs_edge_t;

/**
 * @struct ra8_acmphs_cfg_t
 * @brief Configuration descriptor for ``ra8_acmphs_channel_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_acmphs_channel_init`` in
 * ``libs/ra8_hal/src/ra8_acmphs.c``.
 */
typedef struct {
  uint8_t           ivpsel;     /**< Plus-input select (CMPSEL0).  */
  uint8_t           ivrefsel;   /**< Minus-input select (CMPSEL1). */
  ra8_acmphs_edge_t edge;       /**< Edge detect mode.             */
  bool              filter_en;  /**< True -> enable filter.        */
  bool              invert_out; /**< True -> invert output.        */
} ra8_acmphs_cfg_t;

/**
 * @typedef ra8_acmphs_event_fn_t
 * @brief ACMPHS edge event callback.
 * @param[in] ctx Caller context.
 * @param[in] channel Channel that fired the edge.
 */
typedef void (*ra8_acmphs_event_fn_t)(void* ctx, uint8_t channel);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Legacy init -- reset every channel to its power-on state.
 * @return `k_ra8_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_acmphs_init(void);

/**
 * @brief Initialise one channel with a full config descriptor.
 *
 * @param[in] channel Channel 0..5.
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra8_err_t`` error code.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` has been called.
 * @post CMPCTL / CMPSEL0 / CMPSEL1 / CMPFIR reflect ``cfg``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_acmphs_channel_init(uint8_t channel, const ra8_acmphs_cfg_t* cfg);

/**
 * @brief Tear down one channel (disable + clear CMPCTL).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_acmphs_channel_deinit(uint8_t channel);

/* =============================================================================
 * Legacy polling API
 * =============================================================================
 */

/**
 * @brief Enable a single ACMPHS channel.
 * @param[in] channel Channel index (0..5).
 * @return `k_ra8_ok` on success, `k_ra8_err_invalid_arg` if `channel >= 6`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_acmphs_channel_enable(uint8_t channel);

/**
 * @brief Read the live output of a channel.
 * @param[in] channel Channel index (0..5).
 * @param[out] out Receives `k_ra8_level_high` or `k_ra8_level_low`.
 * @return `ra8_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_acmphs_read_output(uint8_t channel, ra8_level_t* out);

/* =============================================================================
 * Runtime reconfigure
 * =============================================================================
 */

/**
 * @brief Change the input selection of one channel at runtime.
 * @param[in] channel Channel 0..5.
 * @param[in] ivpsel Plus-input selector.
 * @param[in] ivrefsel Minus-input selector.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_acmphs_set_inputs(uint8_t channel, uint8_t ivpsel, uint8_t ivrefsel);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read CMPCTL enable / edge bits.
 * @param[in] channel Channel 0..5.
 * @param[out] out_mask CMPCTL value.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_acmphs_get_status(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Clear CMPCTL (disable + edge mode off) for one channel.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_acmphs_clear_status(uint8_t channel);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach an ACMPHS event callback (shared across all channels).
 * @param[in] fn Callback fired on ISR dispatch.
 * @param[in] ctx Context forwarded to the callback.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_acmphs_attach_handler(ra8_acmphs_event_fn_t fn, void* ctx);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Put one channel into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_acmphs_enter_stop(uint8_t channel);

/**
 * @brief Exit MSTP-gated stop for one channel.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_acmphs_exit_stop(uint8_t channel);

/* =============================================================================
 * ISR dispatch
 * =============================================================================
 */

/**
 * @brief Dispatch an ACMPHS edge event -- fire the registered callback.
 * @param[in] channel Channel 0..5 that fired the edge.
 * @since 0.1.0
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 */
void ra8_acmphs_dispatch(uint8_t channel);

#ifdef __cplusplus
}
#endif

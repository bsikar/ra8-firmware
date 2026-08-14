/**
 * @file ra8_camera_source_ceu.h
 * @brief RA8 CEU capture backend for the transport-neutral camera facade.
 * @ingroup grp_camera
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details Binds the Ring-3 CEU HAL as a `ra8_camera_source_t`. The backend
 * accepts either fixed-size synchronous frames or variable-size JPEG/data
 * streams and always DMA-writes into storage supplied by the caller.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_camera.h"
#include "ra8_ceu.h"
#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Backend configuration, including native CEU register descriptor. */
typedef struct {
  ra8_ceu_config_t         ceu;              /**< HAL descriptor programmed at bind. */
  ra8_camera_source_info_t output;           /**< Frame metadata exposed to callers. */
  uint32_t                 poll_interval_ms; /**< Delay between completion polls.    */
  uint32_t                 poll_attempts;    /**< Maximum completion polls.          */
} ra8_camera_source_ceu_cfg_t;

/**
 * @brief Caller-owned CEU source state.
 *
 * @details Treat fields as private after init. `last_events` retains the most
 *          recent CEU event snapshot even when the backend clears the hardware
 *          latch, allowing diagnostics without bypassing the camera source.
 */
typedef struct {
  ra8_camera_source_info_t info;             /**< Validated output metadata.       */
  ra8_ceu_capture_format_t capture_format;   /**< Fixed-frame or data-enable mode. */
  uint32_t                 poll_interval_ms; /**< Completion polling interval.     */
  uint32_t                 poll_attempts;    /**< Maximum completion polls.        */
  uint32_t                 last_events;      /**< Latest raw CEU event snapshot.   */
  bool                     initialized;      /**< Backend binding completed.       */
} ra8_camera_source_ceu_state_t;

/**
 * @brief Initialize the CEU and bind it as a generic camera source.
 *
 * @details Validates fixed-frame versus data-enable metadata, initializes the
 *          CEU HAL, then binds the facade handle to caller-owned backend state.
 * @param[out] source Generic source handle to bind.
 * @param[out] state Caller-owned backend state.
 * @param[in] cfg CEU and output metadata configuration.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok CEU initialized and source bound.
 * @retval k_ra8_err_null_ptr An argument was `nullptr`.
 * @retval k_ra8_err_invalid_arg Metadata or poll bounds are invalid.
 * @retval other Forwarded from `ra8_ceu_init`.
 * @pre @p source and @p state remain alive for every later source operation.
 * @pre @p cfg describes storage bounds matching its CEU capture format.
 * @post On success @p source dispatches through the CEU backend.
 * @post On success @p state owns all mutable backend state; no heap is used.
 * @note Not thread-safe with respect to the same source, state, or CEU.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_camera_source_ceu_init(ra8_camera_source_t*               source,
                                                   ra8_camera_source_ceu_state_t*     state,
                                                   const ra8_camera_source_ceu_cfg_t* cfg);

/**
 * @brief Read the most recent CEU event snapshot retained by the backend.
 *
 * @details Returns caller-owned diagnostic state only; it does not touch CEU
 *          registers or clear hardware event latches.
 * @param[in] state Initialized caller-owned backend state.
 * @param[out] out_events Receives raw `ra8_ceu_event_t` bits.
 * @return Error code.
 * @retval k_ra8_ok A snapshot was returned.
 * @retval k_ra8_err_null_ptr An argument was `nullptr`.
 * @retval k_ra8_err_not_initialized The backend is not bound.
 * @pre No capture is concurrently mutating @p state.
 * @pre @p out_events points to writable storage.
 * @post Hardware registers are not accessed or modified.
 * @post On success @p out_events equals the last observed raw CEU event bits.
 * @note Thread-safe only while the caller prevents concurrent capture updates.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_camera_source_ceu_get_last_events(const ra8_camera_source_ceu_state_t* state,
                                      uint32_t*                            out_events);

#ifdef __cplusplus
}
#endif

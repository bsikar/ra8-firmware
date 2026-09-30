/**
 * @file ra8_camera_source_ceu_private.h
 * @brief Module-private CEU capture-source seams exported for host coverage.
 * @ingroup grp_camera
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details The CEU backend lives in `src/source_ceu.zig`. Two host suites,
 *          `tests/misc/src/test_ra8_camera.c` and
 *          `tests/misc/src/test_ra8_ceu_cov.c`, drive its bounded completion
 *          poll and its capture entry directly against the fake CEU register
 *          window, so both are exported under the `priv_cam_ceu_` prefix and
 *          declared here. Application code uses `ra8_camera_source_ceu.h`
 *          and the generic `ra8_camera_source_*` facade instead; nothing in
 *          this header is part of the library's public contract.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_camera.h"
#include "ra8_camera_source_ceu.h"
#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Poll bounded CEU status until completion, a fatal fault, or expiry.
 *
 * @details Accumulates diagnostics in @p state while waiting. Sync-timing
 *          events can precede a valid capture-end and therefore do not
 *          terminate the poll; illegal writes, CRAM overflow, invalid vertical
 *          blanking and firewall faults terminate immediately. A data-enable
 *          transfer whose CDSSR is zero reports the configured capacity so a
 *          format-aware caller can locate its own end marker within it.
 * @param[in,out] state Initialized caller-owned CEU backend state.
 * @param[out] out_bytes Captured byte count on completion.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok A complete frame was observed.
 * @retval k_ra8_err_hw_error A fatal CEU event was observed.
 * @retval k_ra8_err_hw_timeout The bounded poll expired.
 * @retval k_ra8_err_not_initialized @p state was `nullptr`.
 * @retval other Propagated CEU status or software-reset error.
 * @pre A capture is armed and @p state is initialized.
 * @pre @p out_bytes points to writable storage.
 * @post @p state retains every observed raw event bit.
 * @post On error or timeout an in-flight capture is software-reset.
 * @note Blocking and not thread-safe with respect to the CEU.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t priv_cam_ceu_wait_for_frame(ra8_camera_source_ceu_state_t* state,
                                                    uint32_t*                      out_bytes);

/**
 * @brief Capture one CEU frame into caller-owned storage.
 *
 * @details The source vtable's capture row, reachable without the facade's own
 *          handle and buffer guards: performs cache maintenance, arms the CEU,
 *          waits, and publishes a frame view of the bytes the peripheral wrote.
 * @param[in,out] state Bound CEU backend state.
 * @param[in] buffer Caller-owned aligned capture storage.
 * @param[out] out_frame Completed immutable frame view.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok One complete frame was captured.
 * @retval k_ra8_err_not_initialized @p state is absent or not initialized.
 * @retval k_ra8_err_invalid_size Capture storage or byte count is invalid.
 * @retval k_ra8_err_invalid_arg Capture storage is not eight-byte aligned.
 * @retval k_ra8_err_null_ptr @p buffer, @p out_frame or the storage was null.
 * @retval other Propagated cache or CEU error.
 * @pre No other operation uses the CEU or the supplied buffer.
 * @post On success @p out_frame views coherent bytes in @p buffer.
 * @post On failure no frame view is published.
 * @note Not thread-safe with respect to the CEU, @p state, or @p buffer.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t priv_cam_ceu_capture(ra8_camera_source_ceu_state_t* state,
                                             const ra8_camera_buffer_t*     buffer,
                                             ra8_camera_frame_t*            out_frame);

#ifdef __cplusplus
}
#endif

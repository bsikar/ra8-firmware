/**
 * @file ra8_camera_ov5640.h
 * @brief OV5640 pairing seam: one open call from sensor mode to bound CEU source.
 * @ingroup grp_camera
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details Pairs an OmniVision OV5640 output mode with the CEU capture
 *          configuration that must receive it. Apps that hand-rolled the
 *          bring-up had to keep a ~20-field `ra8_ceu_config_t` literal in step
 *          with the sensor mode and sequence probe, configure, and CEU bind
 *          themselves; one wrong field or one swapped step was a silent frame
 *          timeout. This seam runs those three stages in order inside one
 *          call and stops at the first failure, leaving the CEU unclaimed
 *          when the sensor never came up.
 *
 *          The board-side parallel-pin routing
 *          (`ra8_board_camera_route_parallel_pins()`) belongs after this call
 *          and before the first capture: it follows the sensor configuration,
 *          and the bound CEU stays idle until the first capture.
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
#include "ra8_ov5640.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Probe the sensor, program one output mode, and bind the CEU capture source.
 *
 * @details Runs probe -> configure -> CEU bind in that order and stops at the
 *          first failure: a probe or configure fault leaves the CEU
 *          unclaimed. The probe verifies the OV5640 chip ID, so callers no
 *          longer re-check it.
 * @param[out] source Generic source handle to bind.
 * @param[out] state Caller-owned CEU backend state.
 * @param[in,out] sensor Sensor instance already bound to its transport
 *                       (`ra8_ov5640_bind_i2c` or `ra8_ov5640_init`).
 * @param[in] mode Validated sensor output mode to program.
 * @param[in] cfg CEU and output metadata configuration paired with @p mode.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Sensor configured and CEU source bound.
 * @retval k_ra8_err_null_ptr An argument was `nullptr`.
 * @retval other Forwarded from `ra8_ov5640_probe`, `ra8_ov5640_configure`, or
 *               `ra8_camera_source_ceu_init`.
 * @pre @p source and @p state remain alive for every later source operation.
 * @pre @p sensor answers SCCB through its bound transport.
 * @post On success @p source dispatches through the CEU backend.
 * @post The board-side parallel-pin routing (e.g.
 *       ::ra8_board_camera_route_parallel_pins) runs after this call and
 *       before the first capture.
 * @note Not thread-safe with respect to the same source, state, or sensor.
 * @note An application calling this must link `ra8_ov5640`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_camera_ov5640_open(ra8_camera_source_t*               source,
                                               ra8_camera_source_ceu_state_t*     state,
                                               ra8_ov5640_t*                      sensor,
                                               ra8_ov5640_mode_t                  mode,
                                               const ra8_camera_source_ceu_cfg_t* cfg);

#ifdef __cplusplus
}
#endif

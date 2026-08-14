/**
 * @file examples/ek_ra8d2/hw_validated/hil/camera_capture/cam_ceu.h
 * @brief App-specific GPIO diagnostic for the parallel DVP camera signals.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Samples the 11 J35 DVP pins as GPIO before the board adapter routes them to
 * CEU. Capture itself uses the reusable `ra8_camera_source_ceu` backend with
 * caller-owned state and storage; this app module retains only the unusually
 * detailed signal-integrity probe needed by the HIL verdict banner.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"

/** @brief Snapshot of DVP sync activity observed through GPIO input samples. */
typedef struct {
  uint32_t vsync_edges;           /**< Observed VIO_VD transitions.                   */
  uint32_t hsync_edges;           /**< Observed VIO_HD transitions.                   */
  uint32_t hsync_high_min;        /**< Shortest observed high run, samples.           */
  uint32_t hsync_high_max;        /**< Longest observed high run, samples.            */
  uint32_t hsync_low_min;         /**< Shortest observed low run, samples.            */
  uint32_t hsync_low_max;         /**< Longest observed low run, samples.             */
  uint32_t hsync_high_cycles_min; /**< Shortest high run, CPU cycles.                 */
  uint32_t hsync_high_cycles_max; /**< Longest high run, CPU cycles.                  */
  uint32_t pclk_edges;            /**< Observed VIO_CLK transitions.                  */
  uint32_t pclk_half_cycles_min;  /**< Shortest observed VIO_CLK half-period.         */
  uint32_t data_samples;          /**< Data bytes sampled on active-line PCLK rises.  */
  uint32_t data_changes;          /**< Changes between consecutive sampled bytes.     */
  uint32_t measured_lines;        /**< Completed HREF pulses measured in PCLK edges.  */
  uint32_t line_pclk_min;         /**< Minimum PCLK rises inside one HREF pulse.      */
  uint32_t line_pclk_max;         /**< Maximum PCLK rises inside one HREF pulse.      */
  uint32_t line_pclk_mean;        /**< Mean PCLK rises inside measured HREF pulses.   */
  uint32_t line_pclk_long;        /**< HREF pulses exceeding 1024 sampled PCLK rises. */
  uint8_t  data_min;              /**< Minimum sampled VIO_D[7:0] value.              */
  uint8_t  data_max;              /**< Maximum sampled VIO_D[7:0] value.              */
  uint8_t  data_and;              /**< AND reduction of sampled VIO_D[7:0].           */
  uint8_t  data_or;               /**< OR reduction of sampled VIO_D[7:0].            */
} cam_ceu_sync_probe_t;

/**
 * @brief Measure DVP sync, clock, and data activity before CEU routing.
 * @details Temporarily samples the camera connector as GPIO to characterize
 *          live sensor timing before the CEU peripheral claims the pins.
 *
 * @param[out] out_probe Receives observed sync transition counts.
 * @return ra8_err_t; ok when the GPIO sampling pass completed.
 * @retval k_ra8_ok Sampling completed and populated `out_probe`.
 * @retval k_ra8_err_null_ptr `out_probe` was null.
 *
 * @pre The sensor is configured and streaming.
 * @pre The DVP pins have not been claimed by
 *      ::ra8_board_camera_route_parallel_pins.
 * @post All temporary DVP GPIO claims are released.
 * @post `out_probe` contains bounded transition and data statistics on success.
 * @note This diagnostic must run before parallel-pin routing.
 * @since 0.1.0
 */
ra8_err_t cam_probe_sync_activity(cam_ceu_sync_probe_t* out_probe);

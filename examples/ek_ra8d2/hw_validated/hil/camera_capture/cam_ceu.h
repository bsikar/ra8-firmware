/**
 * @file examples/ek_ra8d2/hw_validated/hil/camera_capture/cam_ceu.h
 * @brief CEU parallel (DVP) capture: pin routing, open, arm/poll, frame view.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Public contract for the Capture Engine Unit (CEU) half of the camera
 * self-test. The module routes the 11 J35 DVP pins to the CEU, opens the
 * capture engine with a single-shot VGA YUV422 descriptor, captures one frame
 * into a module-owned cache-line-aligned buffer, and lends the app a read-only
 * view of that buffer plus a snapshot of the CEU status register. The raw
 * `ra8_ceu_*` driver and the ::ra8_ceu_config_t descriptor stay inside
 * `src/cam_ceu.c`; callers deal only in these wrappers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum cam_ceu_frame_t
 * @brief Byte count of one captured VGA YUV422 frame.
 * @details Shared between the capture buffer, app plausibility scan, and host
 *          SWD dump, so it is exported as the single source of truth for the
 *          captured byte count.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cam_frame_bytes = 614400U, /**< 640 x 480 x 2 bytes (VGA YUV422). */
} cam_ceu_frame_t;

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
 *
 * @param[out] out_probe Receives observed sync transition counts.
 * @return ra8_err_t; ok when the GPIO sampling pass completed.
 *
 * @pre The sensor is configured and streaming.
 * @pre The DVP pins have not been claimed by ::cam_route_ceu_pins.
 * @post All temporary DVP GPIO claims are released.
 * @since 0.1.0
 */
ra8_err_t cam_probe_sync_activity(cam_ceu_sync_probe_t* out_probe);

/**
 * @brief Route the 11 J35 parallel-camera data/sync pins to the CEU.
 *
 * @details Muxes VIO_D[7:0], VIO_VD, VIO_HD and VIO_CLK from GPIO to the CEU
 *          peripheral function so the capture engine sees the DVP bus.
 *
 * @return ra8_err_t; first failing route or ok.
 * @retval k_ra8_ok All pins routed to the CEU peripheral.
 * @retval k_ra8_err_gpio_conflict A pin was already claimed.
 *
 * @pre `ra8_pfs_init` context (IOPORT reachable).
 * @pre The U15 SW4-6 override selects parallel-camera mode.
 * @post All 11 pins carry the CEU (VIO_*) function (PMR=1, PSEL=CEU).
 * @post No CEU pin is left as GPIO.
 * @note Thread safety: init context only.
 * @since 0.1.0
 */
ra8_err_t cam_route_ceu_pins(void);

/**
 * @brief Fill the CEU descriptor and open the capture engine for a VGA grab.
 *
 * @details Populates the open-time descriptor for a single-shot
 *          data-synchronous 8-bit VGA YUV422 capture and hands it to
 *          `ra8_ceu_init`. Wraps the fill-then-init pair so callers never touch
 *          the raw CEU descriptor type.
 *
 * @return ra8_err_t; ok when the CEU is initialized and idle.
 * @retval k_ra8_ok CEU configured and ready to arm.
 * @retval k_ra8_err_invalid_arg The descriptor was rejected by `ra8_ceu_init`.
 * @retval k_ra8_err_null_ptr Internal descriptor pointer was NULL (cannot occur).
 *
 * @pre The CEU DVP pins are routed (::cam_route_ceu_pins).
 * @pre The sensor is streaming a VGA YUV422 frame.
 * @post The CEU holds the VGA descriptor and is ready for ::cam_capture_one.
 * @post No frame has been captured yet.
 * @note Thread safety: init context only.
 * @since 0.1.0
 */
ra8_err_t cam_ceu_setup(void);

/**
 * @brief Capture one CEU frame into the module buffer, polling CETCR.CPE.
 *
 * @details Arms the capture engine at the internal frame buffer and spins a
 *          bounded poll on the CEU status register until the one-frame-end
 *          (CPE) event latches, then clears it.
 *
 * @return ra8_err_t; ok when the CEU reports one-frame-end.
 * @retval k_ra8_ok Frame captured into the module buffer.
 * @retval k_ra8_err_hw_timeout No CPE within the bounded wait.
 *
 * @pre ::cam_ceu_setup completed and the sensor is streaming.
 * @pre The CEU DVP pins are routed.
 * @post The module frame buffer holds one frame of DVP bytes on success.
 * @post The capture engine is idle (CE cleared).
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
ra8_err_t cam_capture_one(void);

/**
 * @brief Read the CEU transfer-status event register (CETCR) snapshot.
 *
 * @details Thin pass-through to `ra8_ceu_get_status` so the diagnostic CETCR
 *          value (IGHS/VBP/NHD/NVD/CPE bits) can be sampled for the banner
 *          without the caller depending on the CEU driver header.
 *
 * @param[out] out_evt Receives the raw CETCR event bits.
 * @return ra8_err_t from the CEU status read.
 * @retval k_ra8_ok Status sampled into `*out_evt`.
 * @retval k_ra8_err_null_ptr `out_evt` was NULL.
 *
 * @pre `out_evt` is non-NULL.
 * @pre ::cam_ceu_setup has run.
 * @post `*out_evt` holds the CETCR bits on success.
 * @post The CEU status register is left unmodified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
ra8_err_t cam_ceu_get_status(uint32_t* out_evt);

/**
 * @brief Borrow a read-only view of the internal CEU capture buffer.
 *
 * @details Returns the module-owned frame buffer that ::cam_capture_one fills.
 *          The buffer is ::k_cam_frame_bytes long and cache-line aligned for
 *          DMA coherency, exceeding the CEU register's 8-byte requirement.
 *
 * @return const uint8_t* Pointer to the capture buffer (never NULL).
 * @retval non-NULL Always: the buffer has static storage duration.
 *
 * @pre The CEU module is linked into the image.
 * @pre ::cam_capture_one has run for the contents to be meaningful.
 * @post The returned pointer addresses ::k_cam_frame_bytes readable bytes.
 * @post The buffer is not modified by this call.
 * @note Thread safety: returns shared state; read after capture only.
 * @since 0.1.0
 */
const uint8_t* cam_ceu_frame(void);

/** @brief Return the valid byte count of the last completed capture. */
uint32_t cam_ceu_capture_bytes(void);

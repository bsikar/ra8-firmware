/**
 * @file examples/ek_ra8d2/hw_pending/camera_capture/cam_ceu.h
 * @brief CEU parallel (DVP) capture: pin routing, open, arm/poll, frame view.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Public contract for the Capture Engine Unit (CEU) half of the camera
 * self-test. The module routes the 11 J35 DVP pins to the CEU, opens the
 * capture engine with a single-shot QVGA YUV422 descriptor, arms one frame
 * into a module-owned 8-byte-aligned buffer, and lends the app a read-only
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
 * @brief Byte count of one captured QVGA YUV422 frame.
 * @details Shared between the CEU descriptor (`image_area_size`) and the app's
 *          frame-plausibility scan, so it is exported here as the single source
 *          of truth for the capture-buffer length.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cam_frame_bytes = 153600U, /**< 320 x 240 x 2 bytes (QVGA YUV422). */
} cam_ceu_frame_t;

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
 * @pre DIP SW4-6 is ON (board in parallel-camera mode).
 * @post All 11 pins carry the CEU (VIO_*) function (PMR=1, PSEL=CEU).
 * @post No CEU pin is left as GPIO.
 * @note Thread safety: init context only.
 * @since 0.1.0
 */
ra8_err_t cam_route_ceu_pins(void);

/**
 * @brief Fill the CEU descriptor and open the capture engine for a QVGA grab.
 *
 * @details Populates the open-time descriptor for a single-shot
 *          data-synchronous 8-bit QVGA YUV422 capture and hands it to
 *          `ra8_ceu_init`. Wraps the fill-then-init pair so callers never touch
 *          the raw CEU descriptor type.
 *
 * @return ra8_err_t; ok when the CEU is initialized and idle.
 * @retval k_ra8_ok CEU configured and ready to arm.
 * @retval k_ra8_err_invalid_arg The descriptor was rejected by `ra8_ceu_init`.
 * @retval k_ra8_err_null_ptr Internal descriptor pointer was NULL (cannot occur).
 *
 * @pre The CEU DVP pins are routed (::cam_route_ceu_pins).
 * @pre The sensor is streaming a QVGA YUV422 frame.
 * @post The CEU holds the QVGA descriptor and is ready for ::cam_capture_one.
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
 *          The buffer is ::k_cam_frame_bytes long and 8-byte aligned for the
 *          CEU destination-address register.
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

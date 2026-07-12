/**
 * @file examples/ek_ra8d2/hw_pending/camera_capture/cam_ov5640.h
 * @brief OV5640 sensor bring-up over SCCB (reset, chip-ID probe, DVP config).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Public contract for the OV5640 half of the CEU capture self-test. The
 * module owns the SCCB (I2C) conversation with the sensor: releasing the
 * hardware reset strap, discovering which 7-bit address the chip answers on
 * and reading its 16-bit ID, and streaming the compact DVP colour-bar
 * register sequence. The register-init table, the SCCB read/write helpers,
 * and the OV5640 register enums live in `src/cam_ov5640.c`; only the sensor
 * lifecycle entry points and the shared SCCB bus channel are exported here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum cam_iic_t
 * @brief RIIC channel carrying the OV5640 SCCB bus.
 * @details The sensor, the U15 SW4-override expander, and the audio codec all
 *          share this channel, so both the sensor driver and the app-level bus
 *          scan / parallel-mode select refer to it. Exported here as the single
 *          source of truth for the camera I2C channel number.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_cam_iic_ch = 1U, /**< SCL1 P512 / SDA1 P511 (shared with U15 + codec). */
} cam_iic_t;

/**
 * @brief Pulse the OV5640 hardware reset strap (RST, P709, active-low).
 *
 * @details Drives P709 low, holds, then releases it high so the sensor exits
 *          hardware reset with a clean low->high edge. Reset is sampled
 *          against XVCLK, so the caller must bring the clock up first.
 *
 * @return ra8_err_t; ok when RST has been driven low then high.
 * @retval k_ra8_ok Reset pulsed, sensor released.
 * @retval k_ra8_err_gpio_conflict P709 was already claimed.
 *
 * @pre XVCLK is running (reset is sampled against it).
 * @pre P709 is free.
 * @post The sensor has seen a low->high reset edge.
 * @post P709 is driven high (sensor out of reset).
 * @note Thread safety: init context only.
 * @since 0.1.0
 */
ra8_err_t cam_reset_sensor(void);

/**
 * @brief Read the OV5640 chip ID, trying SCCB address 0x3C then 0x3D.
 *
 * @details Reads registers 0x300A/0x300B on each candidate address and, on the
 *          first that returns 0x5640, leaves the module's active SCCB address
 *          set to it so subsequent writes target the right device.
 *
 * @param[out] out_id Receives the last chip ID actually read (0 if none).
 * @return true when 0x5640 was read (and the active SCCB address is left there).
 * @retval true  Sensor present; active SCCB address is where it answered.
 * @retval false No 0x5640 seen; active SCCB address restored to 0x3C.
 *
 * @pre `out_id` is non-NULL.
 * @pre XVCLK is running and the sensor is out of hardware reset.
 * @post On true, the active SCCB address is the one the OV5640 answered on.
 * @post On false, the active SCCB address is restored to 0x3C.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
bool cam_probe_sensor(uint16_t* out_id);

/**
 * @brief Software-reset the OV5640 and apply the DVP colour-bar sequence.
 *
 * @details Issues an SCCB software reset, streams the trimmed DVP YUV422 QVGA
 *          colour-bar register table, then wakes the sensor into normal
 *          operation so it drives a deterministic frame on the parallel bus.
 *
 * @return ra8_err_t; ok when every register write ACKed.
 * @retval k_ra8_ok Sensor configured and woken.
 * @retval k_ra8_err_nack A register write was NACKed.
 *
 * @pre RIIC ch1 up, XVCLK running, sensor out of hardware reset.
 * @pre The chip ID has been confirmed as 0x5640.
 * @post The sensor streams a QVGA YUV422 colour bar on the DVP bus.
 * @post The sensor is in normal (awake) mode.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
ra8_err_t cam_configure_sensor(void);

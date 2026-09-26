/**
 * @file ra8_board_ek_ra8d2_camera_mode.h
 * @brief EK-RA8D2 J35 camera capture policy: sensor mode to CEU descriptor.
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Kept separate from the connector-only header so consumers that need
 * board pin constants do not inherit the CEU HAL dependency, the same
 * split ``ra8_board_ek_ra8d2_pdm.h`` makes for the microphone.
 *
 * The board owns the twenty-odd register fields that say "OV5640, VGA,
 * packed UYVY, 8-bit DVP" so an application no longer re-derives them
 * from whichever sibling it was copied from. The caller still owns its
 * frame storage, so the buffer bound is an argument rather than board
 * policy.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_ceu.h"
#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Capture modes the J35 OV5640 camera module is validated in. */
typedef enum : uint8_t {
  k_ra8_board_camera_mode_vga_uyvy = 0U, /**< 640x480 packed UYVY, synchronous. */
  k_ra8_board_camera_mode_vga_jpeg,      /**< 640x480 sensor JPEG, data-enable. */
  k_ra8_board_camera_mode_count,         /**< Number of validated modes.        */
} ra8_board_camera_mode_t;

/** @brief Board-owned capture policy for one camera mode. */
typedef struct {
  ra8_ceu_config_t ceu;              /**< HAL descriptor programmed at bind. */
  uint32_t         frame_bytes_max;  /**< Worst-case captured frame bytes.   */
  uint32_t         stride_bytes;     /**< Output row pitch; 0 when gated.    */
  uint32_t         poll_interval_ms; /**< Suggested completion poll period.  */
  uint32_t         poll_attempts;    /**< Suggested completion poll limit.   */
  uint32_t         xclk_hz;          /**< Sensor input clock for this mode.  */
  uint32_t         settle_ms;        /**< Delay after routing before use.    */
  uint16_t         width_px;         /**< Native output width.               */
  uint16_t         height_px;        /**< Native output height.              */
} ra8_board_camera_ceu_config_t;

/**
 * @brief Return the validated CEU capture policy for one board camera mode.
 *
 * @details
 * Mirrors ``ra8_board_pdm_mic_get_config``: the BSP hands over the whole
 * HAL descriptor so the application never types sync polarities, latch
 * edges, byte swaps, input order or burst size. The returned descriptor
 * is the bench-proven one both C6 camera applications carry today.
 *
 * @param[in] mode Validated capture mode selector.
 * @param[in] frame_bytes_max Capacity of the caller's frame buffer, in bytes.
 * @param[out] out_config Receives geometry, timing, and the CEU descriptor.
 * @return Error code.
 * @retval k_ra8_ok Configuration returned.
 * @retval k_ra8_err_null_ptr `out_config` was `nullptr`.
 * @retval k_ra8_err_invalid_arg `mode` is out of range.
 * @retval k_ra8_err_invalid_size `frame_bytes_max` cannot hold one frame.
 * @pre `out_config` points to writable storage.
 * @pre The J35 parallel path is selected before the descriptor is programmed.
 * @post On success `out_config` describes 640x480 capture in the named format.
 * @post No CEU, pin, or sensor register is accessed.
 * @note Thread-safe; returns immutable board policy by value.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_camera_get_ceu_config(
  ra8_board_camera_mode_t mode, uint32_t frame_bytes_max,
  ra8_board_camera_ceu_config_t* out_config);

#ifdef __cplusplus
}
#endif

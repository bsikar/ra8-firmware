/**
 * @file ra8_board_ek_ra8d2_camera_mode.c
 * @brief EK-RA8D2 J35 camera capture policy for the validated OV5640 modes.
 * @ingroup grp_board
 * @details Holds the CEU register descriptor each validated sensor mode
 *          implies, so an application states the mode and not the twenty
 *          fields behind it. No register is touched here.
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2_camera_mode.h"
#include "ra8_ceu.h"
#include "ra8_err.h"

typedef enum : uint16_t {
  k_board_camera_vga_width  = 640U, /**< OV5640 VGA output width.  */
  k_board_camera_vga_height = 480U, /**< OV5640 VGA output height. */
} ra8_board_camera_geometry_t;

typedef enum : uint8_t {
  k_board_camera_uyvy_bytes_per_px = 2U, /**< Packed UYVY sample pair. */
  k_board_camera_jpeg_bytes_per_px = 1U, /**< Gated byte stream.       */
} ra8_board_camera_depth_t;

typedef enum : uint32_t {
  k_board_camera_xclk_hz     = 24000000U, /**< OV5640 input clock.       */
  k_board_camera_settle_ms   = 100U,      /**< Routing settle delay.     */
  k_board_camera_uyvy_poll_ms    = 5U,    /**< Synchronous poll period.  */
  k_board_camera_uyvy_poll_tries = 800U,  /**< Synchronous poll limit.   */
  k_board_camera_jpeg_poll_ms    = 2U,    /**< Gated-stream poll period. */
  k_board_camera_jpeg_poll_tries = 2000U, /**< Gated-stream poll limit.  */
} ra8_board_camera_timing_t;

/**
 * @brief Build the packed-UYVY capture policy.
 *
 * @details
 * Synchronous fetch of one 640x480 frame, 8-bit DVP, high-active syncs,
 * every signal latched on the rising VIO_CLK edge, 32-byte bus bursts,
 * word and dword swaps set so memory holds the UYVY order the software
 * converter reads. The scale block is clip-only, no scale-down.
 *
 * @param[in] frame_bytes_max Caller frame-buffer capacity, already validated.
 * @return Complete board camera policy for this mode.
 * @retval ra8_board_camera_ceu_config_t Populated by value.
 * @pre `frame_bytes_max` holds at least one packed frame.
 * @post The returned descriptor references no caller storage.
 * @note Pure; no hardware access.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_board_camera_ceu_config_t
internal_board_camera_vga_uyvy(uint32_t frame_bytes_max)
{
  const uint16_t stride =
    (uint16_t)((uint16_t)k_board_camera_vga_width * (uint16_t)k_board_camera_uyvy_bytes_per_px);
  const ra8_ceu_config_t ceu = {
    .width_px        = (uint16_t)k_board_camera_vga_width,
    .height_px       = (uint16_t)k_board_camera_vga_height,
    .x_capture_px    = stride,
    .y_capture_lines = (uint16_t)k_board_camera_vga_height,
    .dst_stride      = stride,
    .bytes_per_pixel = (uint8_t)k_board_camera_uyvy_bytes_per_px,
    .capture_format  = k_ra8_ceu_fmt_data_synchronous,
    .capture_mode    = k_ra8_ceu_capture_single,
    .data_bus        = k_ra8_ceu_bus_8_bit,
    .hsync_polarity  = k_ra8_ceu_pol_high_active,
    .vsync_polarity  = k_ra8_ceu_pol_high_active,
    .field_polarity  = k_ra8_ceu_pol_high_active,
    .input_order     = k_ra8_ceu_input_cb0_y0_cr0_y1,
    .output_format   = k_ra8_ceu_output_ycbcr_422,
    .burst_mode      = k_ra8_ceu_burst_32,
    .first_field     = k_ra8_ceu_field_immediate,
    .edge            = {k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising},
    .byte_swap       = {false, true, true},
    .scale           = {0U,
                        0U,
                        0U,
                        0U,
                        (uint16_t)k_board_camera_vga_width,
                        (uint16_t)k_board_camera_vga_height},
  };
  return (ra8_board_camera_ceu_config_t){
    .ceu              = ceu,
    .frame_bytes_max  = frame_bytes_max,
    .stride_bytes     = (uint32_t)stride,
    .poll_interval_ms = (uint32_t)k_board_camera_uyvy_poll_ms,
    .poll_attempts    = (uint32_t)k_board_camera_uyvy_poll_tries,
    .xclk_hz          = (uint32_t)k_board_camera_xclk_hz,
    .settle_ms        = (uint32_t)k_board_camera_settle_ms,
    .width_px         = (uint16_t)k_board_camera_vga_width,
    .height_px        = (uint16_t)k_board_camera_vga_height,
  };
}

/**
 * @brief Build the sensor-JPEG capture policy.
 *
 * @details
 * Data-enable fetch of a gated byte stream whose length is not known
 * until the frame ends, so the caller's capacity becomes the firewall
 * window (`image_area_size`) and the reported row pitch is zero. All
 * three byte swaps are set, the shape the hardware-JPEG reader expects,
 * and bursts are 256 bytes because the stream is not line-structured.
 *
 * @param[in] frame_bytes_max Caller frame-buffer capacity, already validated.
 * @return Complete board camera policy for this mode.
 * @retval ra8_board_camera_ceu_config_t Populated by value.
 * @pre `frame_bytes_max` is non-zero.
 * @post `image_area_size` equals the caller's capacity.
 * @note Pure; no hardware access.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_board_camera_ceu_config_t
internal_board_camera_vga_jpeg(uint32_t frame_bytes_max)
{
  const ra8_ceu_config_t ceu = {
    .width_px        = (uint16_t)k_board_camera_vga_width,
    .height_px       = (uint16_t)k_board_camera_vga_height,
    .x_capture_px    = (uint16_t)k_board_camera_vga_width,
    .y_capture_lines = (uint16_t)k_board_camera_vga_height,
    .dst_stride      = (uint16_t)k_board_camera_vga_width,
    .bytes_per_pixel = (uint8_t)k_board_camera_jpeg_bytes_per_px,
    .capture_format  = k_ra8_ceu_fmt_data_enable,
    .capture_mode    = k_ra8_ceu_capture_single,
    .data_bus        = k_ra8_ceu_bus_8_bit,
    .hsync_polarity  = k_ra8_ceu_pol_high_active,
    .vsync_polarity  = k_ra8_ceu_pol_high_active,
    .field_polarity  = k_ra8_ceu_pol_high_active,
    .input_order     = k_ra8_ceu_input_cb0_y0_cr0_y1,
    .output_format   = k_ra8_ceu_output_ycbcr_422,
    .burst_mode      = k_ra8_ceu_burst_256,
    .first_field     = k_ra8_ceu_field_immediate,
    .edge            = {k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising},
    .byte_swap       = {true, true, true},
    .image_area_size = frame_bytes_max,
  };
  return (ra8_board_camera_ceu_config_t){
    .ceu              = ceu,
    .frame_bytes_max  = frame_bytes_max,
    .stride_bytes     = 0U,
    .poll_interval_ms = (uint32_t)k_board_camera_jpeg_poll_ms,
    .poll_attempts    = (uint32_t)k_board_camera_jpeg_poll_tries,
    .xclk_hz          = (uint32_t)k_board_camera_xclk_hz,
    .settle_ms        = (uint32_t)k_board_camera_settle_ms,
    .width_px         = (uint16_t)k_board_camera_vga_width,
    .height_px        = (uint16_t)k_board_camera_vga_height,
  };
}

/* See the public header for the documented contract. */
ra8_err_t ra8_board_camera_get_ceu_config(ra8_board_camera_mode_t        mode,
                                          uint32_t                       frame_bytes_max,
                                          ra8_board_camera_ceu_config_t* out_config)
{
  if (out_config == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((uint8_t)mode >= (uint8_t)k_ra8_board_camera_mode_count) {
    return k_ra8_err_invalid_arg;
  }
  if (frame_bytes_max == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (mode == k_ra8_board_camera_mode_vga_uyvy) {
    const uint32_t packed_bytes = (uint32_t)k_board_camera_vga_width *
                                  (uint32_t)k_board_camera_vga_height *
                                  (uint32_t)k_board_camera_uyvy_bytes_per_px;
    if (frame_bytes_max < packed_bytes) {
      return k_ra8_err_invalid_size;
    }
    *out_config = internal_board_camera_vga_uyvy(frame_bytes_max);
    return k_ra8_ok;
  }
  *out_config = internal_board_camera_vga_jpeg(frame_bytes_max);
  return k_ra8_ok;
}

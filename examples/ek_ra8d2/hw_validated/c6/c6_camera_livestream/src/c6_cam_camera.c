/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_camera_livestream/src/c6_cam_camera.c
 * @brief Camera initialization and VGA UYVY to QVGA JPEG frame production.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details The CEU keeps the bench-proven 640x480 packed UYVY configuration.
 * Each browser request captures a complete frame, downsamples by two in both
 * axes while converting BT.601 YCbCr to RGB888, then uses the repository's
 * baseline software JPEG encoder. The CEU, RGB and JPEG buffers live in
 * external SDRAM so the NetX working set fits in internal SRAM.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "c6_camera_server.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_camera.h"
#include "ra8_camera_codec_jpeg_sw.h"
#include "ra8_camera_source_ceu.h"
#include "ra8_ceu.h"
#include "ra8_ov5640.h"
#include "ra8_time.h"

/** @brief Software-camera backend geometry, buffers, and sensor timing. */
typedef enum : uint32_t {
  /** @brief OV5640 capture width. */
  k_sw_cam_source_width = 640U,
  /** @brief OV5640 capture height. */
  k_sw_cam_source_height = 480U,
  /** @brief Packed UYVY row bytes. */
  k_sw_cam_source_stride = k_sw_cam_source_width * 2U,
  /** @brief Packed UYVY frame bytes. */
  k_sw_cam_source_bytes = k_sw_cam_source_stride * k_sw_cam_source_height,
  /** @brief Encoded JPEG width. */
  k_sw_cam_stream_width = 320U,
  /** @brief Encoded JPEG height. */
  k_sw_cam_stream_height = 240U,
  /** @brief RGB conversion workspace bytes. */
  k_sw_cam_rgb_bytes = k_sw_cam_stream_width * k_sw_cam_stream_height * 3U,
  /** @brief Worst-case JPEG buffer bytes. */
  k_sw_cam_jpeg_bytes = k_sw_cam_rgb_bytes,
  /** @brief Software JPEG quality. */
  k_sw_cam_jpeg_quality = 65U,
  /** @brief OV5640 input clock. */
  k_sw_cam_xclk_hz = 24000000U,
  /** @brief Sensor routing settle delay. */
  k_sw_cam_mode_settle_ms = 100U,
  /** @brief CEU completion poll period. */
  k_sw_cam_poll_ms = 5U,
  /** @brief CEU completion poll limit. */
  k_sw_cam_poll_attempts = 800U,
} sw_cam_cfg_t;

/** @brief Caller-selected SDRAM buffers for capture, conversion, and JPEG. */
[[gnu::section(".sdram_data"), gnu::aligned(32)]] static uint8_t s_capture[k_sw_cam_source_bytes];
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t  s_rgb[k_sw_cam_rgb_bytes];
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t  s_jpeg[k_sw_cam_jpeg_bytes];
/** @brief Transport-independent OV5640 instance bound to the EK-RA8D2 SCCB bus. */
static ra8_ov5640_t                     s_camera_sensor;
static ra8_camera_source_t              s_source;
static ra8_camera_source_ceu_state_t    s_source_state;
static ra8_camera_codec_t               s_codec;
static ra8_camera_codec_jpeg_sw_state_t s_codec_state;

/**
 * @brief Bind the software-JPEG sensor instance to board SCCB callbacks.
 * @details Constructs the transport used by the reusable OV5640 driver.
 * @return Repository error code.
 * @retval k_ra8_ok The sensor instance was initialized.
 * @retval k_ra8_err_null_ptr A required transport dependency was absent.
 * @pre Board camera support is linked into the application.
 * @pre No capture concurrently uses `s_camera_sensor`.
 * @post On success, the sensor instance selects the primary SCCB address.
 * @post Binding performs no sensor register transfer.
 * @note Probing occurs during camera initialization.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_c6_cam_sensor_bind(void)
{
  const ra8_ov5640_bus_t bus = {
    .read_reg  = ra8_board_camera_sccb_read_reg,
    .write_reg = ra8_board_camera_sccb_write_reg,
    .delay_ms  = ra8_board_camera_delay_ms,
    .ctx       = nullptr,
  };
  return ra8_ov5640_init(&s_camera_sensor, &bus);
}

/**
 * @brief Build the CEU configuration for packed VGA capture.
 * @details Selects one-shot synchronous UYVY output into the static source buffer.
 * @return Generic CEU source configuration.
 * @retval ra8_camera_source_ceu_cfg_t Complete packed-camera source configuration.
 * @pre Geometry constants match the configured OV5640 VGA mode.
 * @pre The static capture buffer can hold the reported maximum frame bytes.
 * @post Returned configuration references no temporary storage.
 * @post Static source and codec state remain unchanged.
 * @note Software conversion and JPEG encoding occur after CEU capture.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_camera_source_ceu_cfg_t internal_c6_cam_source_config(void)
{
  const ra8_ceu_config_t ceu = {
    .width_px        = (uint16_t)k_sw_cam_source_width,
    .height_px       = (uint16_t)k_sw_cam_source_height,
    .x_capture_px    = (uint16_t)k_sw_cam_source_stride,
    .y_capture_lines = (uint16_t)k_sw_cam_source_height,
    .dst_stride      = (uint16_t)k_sw_cam_source_stride,
    .bytes_per_pixel = 2U,
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
    .scale = {0U, 0U, 0U, 0U, (uint16_t)k_sw_cam_source_width, (uint16_t)k_sw_cam_source_height},
  };
  return (ra8_camera_source_ceu_cfg_t){
    .ceu = ceu,
    .output =
      {
        .frame_bytes_max = (uint32_t)k_sw_cam_source_bytes,
        .stride_bytes    = (uint32_t)k_sw_cam_source_stride,
        .width           = (uint16_t)k_sw_cam_source_width,
        .height          = (uint16_t)k_sw_cam_source_height,
        .format          = k_ra8_camera_format_uyvy422,
      },
    .poll_interval_ms = (uint32_t)k_sw_cam_poll_ms,
    .poll_attempts    = (uint32_t)k_sw_cam_poll_attempts,
  };
}

ra8_err_t c6_cam_camera_init(void)
{
  ra8_err_t err = ra8_board_camera_xclk_start((uint32_t)k_sw_cam_xclk_hz);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_board_camera_select_parallel();
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms((uint32_t)k_sw_cam_mode_settle_ms);
  err = ra8_board_camera_reset();
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_c6_cam_sensor_bind();
  if (err != k_ra8_ok) {
    return err;
  }
  uint16_t chip_id = 0U;
  if ((ra8_ov5640_probe(&s_camera_sensor, &chip_id) != k_ra8_ok) ||
      (chip_id != (uint16_t)k_ra8_ov5640_chip_id)) {
    return k_ra8_err_hw_init_failed;
  }
  err = ra8_ov5640_configure(&s_camera_sensor, k_ra8_ov5640_mode_vga_uyvy);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_board_camera_route_parallel_pins();
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_camera_source_ceu_cfg_t source_cfg = internal_c6_cam_source_config();
  err = ra8_camera_source_ceu_init(&s_source, &s_source_state, &source_cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_camera_codec_jpeg_sw_cfg_t codec_cfg = {
    .rgb_workspace          = s_rgb,
    .rgb_workspace_capacity = (uint32_t)sizeof(s_rgb),
    .output_width           = (uint16_t)k_sw_cam_stream_width,
    .output_height          = (uint16_t)k_sw_cam_stream_height,
    .quality                = (uint8_t)k_sw_cam_jpeg_quality,
  };
  return ra8_camera_codec_jpeg_sw_init(&s_codec, &s_codec_state, &codec_cfg);
}

ra8_err_t c6_cam_camera_capture_jpeg(const uint8_t**        out_jpeg,
                                     uint32_t*              out_bytes,
                                     c6_cam_frame_timing_t* out_timing)
{
  if ((out_jpeg == nullptr) || (out_bytes == nullptr) || (out_timing == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_jpeg                                = nullptr;
  *out_bytes                               = 0U;
  *out_timing                              = (c6_cam_frame_timing_t){};
  const uint32_t capture_start             = ra8_time_ms();
  out_timing->timestamp_ms                 = capture_start;
  const ra8_camera_buffer_t capture_buffer = {
    .data     = s_capture,
    .capacity = (uint32_t)sizeof(s_capture),
  };
  ra8_camera_frame_t captured    = {};
  ra8_err_t          err         = ra8_camera_source_capture(&s_source, &capture_buffer, &captured);
  const uint32_t     capture_end = ra8_time_ms();
  out_timing->capture_ms         = capture_end - capture_start;
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_camera_buffer_t encoded_buffer = {
    .data     = s_jpeg,
    .capacity = (uint32_t)sizeof(s_jpeg),
  };
  ra8_camera_frame_t encoded = {};
  err                   = ra8_camera_codec_encode(&s_codec, &captured, &encoded_buffer, &encoded);
  out_timing->encode_ms = ra8_time_ms() - capture_end;
  if (err != k_ra8_ok) {
    return err;
  }
  *out_jpeg  = encoded.data;
  *out_bytes = encoded.bytes;
  return k_ra8_ok;
}

const char* c6_cam_camera_description(void)
{
  return "frame=640x480 UYVY software-codec=320x240 JPEG";
}

void c6_cam_camera_report_last_error(void)
{
  uint32_t events = 0U;
  if (ra8_camera_source_ceu_get_last_events(&s_source_state, &events) == k_ra8_ok) {
    c6_cam_puts(" ceu_events=");
    c6_cam_put_u32(events);
  }
}

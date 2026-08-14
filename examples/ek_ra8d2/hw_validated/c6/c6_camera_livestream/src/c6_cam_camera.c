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

#include "c6_camera_livestream.h"
#include "cam_ceu.h"
#include "cam_ov5640.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_gpt.h"
#include "ra8_jpeg_sw.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"

/** @brief GPT channel driving the OV5640 XVCLK input on P501. */
typedef enum : uint8_t {
  k_c6_cam_xclk_gpt_ch = 12U,
} c6_cam_gpt_t;

/** @brief U15 address/register and the combined C6 + DVP switch pattern. */
typedef enum : uint8_t {
  k_c6_cam_sw46_output = 0xDFU,
  k_c6_cam_sw46_mask   = 0x20U,
} c6_cam_u15_t;

/** @brief BT.601 fixed-point conversion constants and sensor identity. */
typedef enum : int32_t {
  k_c6_cam_u8_max         = 255,
  k_c6_cam_chroma_neutral = 128,
  k_c6_cam_luma_scale     = 298,
  k_c6_cam_red_cr_scale   = 409,
  k_c6_cam_green_cb_scale = 100,
  k_c6_cam_green_cr_scale = 208,
  k_c6_cam_blue_cb_scale  = 516,
  k_c6_cam_rounding       = 128,
  k_c6_cam_sensor_id      = 0x5640,
} c6_cam_color_t;

/** @brief QVGA RGB work buffer. */
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_rgb[k_c6_cam_rgb_bytes];
/** @brief Encoded JPEG returned to the HTTP server. */
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_jpeg[k_c6_cam_jpeg_bytes];

static ra8_err_t c6_cam_start_xclk(void)
{
  uint32_t  pclkd_hz = 0U;
  ra8_err_t err      = ra8_cgc_get_clock_hz(k_ra8_clock_id_pclkd, &pclkd_hz);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t period = pclkd_hz / (uint32_t)k_c6_cam_xclk_hz;
  if ((period < 2U) || (period > (uint32_t)k_c6_cam_gpt_period_max)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_gpt_cfg_t cfg = {
    .mode       = k_ra8_gpt_mode_saw_pwm,
    .prescaler  = k_ra8_gpt_ps_div_1,
    .period     = period - 1U,
    .duty_a     = period / 2U,
    .duty_b     = 0U,
    .auto_start = true,
  };
  err = ra8_gpt_init((uint8_t)k_c6_cam_xclk_gpt_ch, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_gpt_pwm_pin_cfg_t pin = {
    .output_enable    = true,
    .polarity         = k_ra8_gpt_pol_active_high,
    .stop_level       = k_ra8_gpt_stop_low,
    .disable_on_fault = k_ra8_gpt_disable_none,
  };
  err = ra8_gpt_pwm_pin_configure((uint8_t)k_c6_cam_xclk_gpt_ch, k_ra8_gpt_pin_a, &pin);
  if (err != k_ra8_ok) {
    return err;
  }
  err =
    ra8_pfs_route_peripheral(RA8_PIN(k_ra8_port_5, k_ra8_pin_1), k_ra8_psel_gpt0, "c6_cam.xclk");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_pfs_set_drive_strength(RA8_PIN(k_ra8_port_5, k_ra8_pin_1),
                                    k_ra8_pfs_dscr_high_speed_high);
}

static ra8_err_t c6_cam_select_switches(void)
{
  return ra8_board_io_expander_apply_sw4_mask((uint8_t)k_c6_cam_sw46_output,
                                              (uint8_t)k_c6_cam_sw46_mask);
}

static uint8_t c6_cam_clamp(int32_t value)
{
  if (value < 0) {
    return 0U;
  }
  if (value > k_c6_cam_u8_max) {
    return (uint8_t)k_c6_cam_u8_max;
  }
  return (uint8_t)value;
}

static void c6_cam_uyvy_to_qvga_rgb(const uint8_t* source)
{
  for (uint32_t y = 0U; y < (uint32_t)k_c6_cam_stream_height; y++) {
    const uint32_t source_row = (y * 2U) * (uint32_t)k_c6_cam_source_stride;
    const uint32_t output_row = y * (uint32_t)k_c6_cam_stream_width * 3U;
    for (uint32_t x = 0U; x < (uint32_t)k_c6_cam_stream_width; x++) {
      const uint32_t pair  = source_row + (x * 4U);
      const int32_t  cb    = (int32_t)source[pair] - k_c6_cam_chroma_neutral;
      const int32_t  yy    = (int32_t)source[pair + 1U] - 16;
      const int32_t  cr    = (int32_t)source[pair + 2U] - k_c6_cam_chroma_neutral;
      const int32_t  lum   = (yy < 0) ? 0 : (k_c6_cam_luma_scale * yy);
      const uint32_t pixel = output_row + (x * 3U);
      s_rgb[pixel] = c6_cam_clamp((lum + (k_c6_cam_red_cr_scale * cr) + k_c6_cam_rounding) >> 8);
      s_rgb[pixel + 1U] = c6_cam_clamp((lum - (k_c6_cam_green_cb_scale * cb) -
                                        (k_c6_cam_green_cr_scale * cr) + k_c6_cam_rounding) >>
                                       8);
      s_rgb[pixel + 2U] =
        c6_cam_clamp((lum + (k_c6_cam_blue_cb_scale * cb) + k_c6_cam_rounding) >> 8);
    }
  }
}

ra8_err_t c6_cam_camera_init(void)
{
  ra8_err_t err = c6_cam_start_xclk();
  if (err != k_ra8_ok) {
    return err;
  }
  err = c6_cam_select_switches();
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms((uint32_t)k_c6_cam_mode_settle_ms);
  err = cam_reset_sensor();
  if (err != k_ra8_ok) {
    return err;
  }
  uint16_t chip_id = 0U;
  if (!cam_probe_sensor(&chip_id) || (chip_id != (uint16_t)k_c6_cam_sensor_id)) {
    return k_ra8_err_hw_init_failed;
  }
  err = cam_configure_sensor();
  if (err != k_ra8_ok) {
    return err;
  }
  err = cam_route_ceu_pins();
  if (err != k_ra8_ok) {
    return err;
  }
  return cam_ceu_setup();
}

ra8_err_t c6_cam_camera_capture_jpeg(const uint8_t** out_jpeg, uint32_t* out_bytes)
{
  if ((out_jpeg == nullptr) || (out_bytes == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_jpeg     = nullptr;
  *out_bytes    = 0U;
  ra8_err_t err = cam_capture_one();
  if (err != k_ra8_ok) {
    return err;
  }
  if (cam_ceu_capture_bytes() != (uint32_t)k_cam_frame_bytes) {
    return k_ra8_err_invalid_size;
  }
  c6_cam_uyvy_to_qvga_rgb(cam_ceu_frame());
  err = ra8_jpeg_sw_encode(s_rgb,
                           (uint16_t)k_c6_cam_stream_width,
                           (uint16_t)k_c6_cam_stream_height,
                           (uint8_t)k_c6_cam_jpeg_quality,
                           s_jpeg,
                           (uint32_t)sizeof(s_jpeg),
                           out_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_jpeg = s_jpeg;
  return k_ra8_ok;
}

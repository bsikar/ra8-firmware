/**
 * @file examples/ek_ra8d2/hw_validated/hil/camera_capture/src/cam_image.c
 * @brief UYVY-to-RGB888 conversion and 0/90/180/270 SDRAM image production.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "cam_image.h"

#include <stdint.h>

#include "cam_ceu.h"
#include "ra8_cache.h"
#include "ra8_check.h"

/** @brief Cache-line alignment shared by the SDRAM buffers and cache API. */
typedef enum : uint32_t {
  k_cam_image_cache_line_bytes = 32U,
} cam_image_align_t;

/** @brief BT.601 limited-range fixed-point YCbCr-to-RGB coefficients. */
typedef enum : int32_t {
  k_cam_yuv_luma_black = 16,
  k_cam_yuv_chroma_mid = 128,
  k_cam_yuv_luma_scale = 298,
  k_cam_yuv_red_cr     = 409,
  k_cam_yuv_green_cb   = 100,
  k_cam_yuv_green_cr   = 208,
  k_cam_yuv_blue_cb    = 516,
  k_cam_yuv_rounding   = 128,
  k_cam_yuv_shift      = 8,
} cam_image_yuv_coeff_t;

/** @brief One RGB888 pixel used while scattering the four orientations. */
typedef struct {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
} cam_rgb_t;

[[gnu::section(".sdram_data"), gnu::aligned(k_cam_image_cache_line_bytes)]]
uint8_t g_cam_rgb_0[k_cam_rgb_frame_bytes];
[[gnu::section(".sdram_data"), gnu::aligned(k_cam_image_cache_line_bytes)]]
uint8_t g_cam_rgb_90[k_cam_rgb_frame_bytes];
[[gnu::section(".sdram_data"), gnu::aligned(k_cam_image_cache_line_bytes)]]
uint8_t g_cam_rgb_180[k_cam_rgb_frame_bytes];
[[gnu::section(".sdram_data"), gnu::aligned(k_cam_image_cache_line_bytes)]]
uint8_t g_cam_rgb_270[k_cam_rgb_frame_bytes];

const uint32_t g_cam_rgb_frame_bytes = (uint32_t)k_cam_rgb_frame_bytes;

/** @brief Clamp a signed RGB component to one byte. */
static uint8_t cam_image_clamp(int32_t component)
{
  if (component < 0) {
    return 0U;
  }
  if (component > UINT8_MAX) {
    return UINT8_MAX;
  }
  return (uint8_t)component;
}

/** @brief Convert one limited-range YCbCr pixel to RGB888. */
static cam_rgb_t cam_image_ycbcr_to_rgb(uint8_t y, uint8_t cb, uint8_t cr)
{
  int32_t luma = (int32_t)y - (int32_t)k_cam_yuv_luma_black;
  if (luma < 0) {
    luma = 0;
  }
  const int32_t   blue_delta = (int32_t)cb - (int32_t)k_cam_yuv_chroma_mid;
  const int32_t   red_delta  = (int32_t)cr - (int32_t)k_cam_yuv_chroma_mid;
  const cam_rgb_t rgb        = {
    .red = cam_image_clamp(((int32_t)k_cam_yuv_luma_scale * luma +
                            (int32_t)k_cam_yuv_red_cr * red_delta + (int32_t)k_cam_yuv_rounding) >>
                           (int32_t)k_cam_yuv_shift),
    .green = cam_image_clamp(
      ((int32_t)k_cam_yuv_luma_scale * luma - (int32_t)k_cam_yuv_green_cb * blue_delta -
       (int32_t)k_cam_yuv_green_cr * red_delta + (int32_t)k_cam_yuv_rounding) >>
      (int32_t)k_cam_yuv_shift),
    .blue =
      cam_image_clamp(((int32_t)k_cam_yuv_luma_scale * luma +
                       (int32_t)k_cam_yuv_blue_cb * blue_delta + (int32_t)k_cam_yuv_rounding) >>
                      (int32_t)k_cam_yuv_shift),
  };
  return rgb;
}

/** @brief Write one RGB pixel to all four rotated row-major destinations. */
static void cam_image_scatter(uint32_t x, uint32_t y, cam_rgb_t rgb)
{
  const uint32_t width  = (uint32_t)k_cam_image_width_px;
  const uint32_t height = (uint32_t)k_cam_image_height_px;
  const uint32_t idx_0  = ((y * width) + x) * (uint32_t)k_cam_rgb_bytes_per_px;
  const uint32_t idx_90 = ((x * height) + (height - 1U - y)) * (uint32_t)k_cam_rgb_bytes_per_px;
  const uint32_t idx_180 =
    (((height - 1U - y) * width) + (width - 1U - x)) * (uint32_t)k_cam_rgb_bytes_per_px;
  const uint32_t idx_270    = (((width - 1U - x) * height) + y) * (uint32_t)k_cam_rgb_bytes_per_px;
  uint8_t* const outputs[4] = {
    &g_cam_rgb_0[idx_0],
    &g_cam_rgb_90[idx_90],
    &g_cam_rgb_180[idx_180],
    &g_cam_rgb_270[idx_270],
  };
  for (uint32_t output = 0U; output < 4U; output += 1U) {
    outputs[output][0] = rgb.red;
    outputs[output][1] = rgb.green;
    outputs[output][2] = rgb.blue;
  }
}

ra8_err_t cam_image_generate(const uint8_t* uyvy, uint32_t source_bytes)
{
  RA8_CHECK_NULL_PTR(uyvy, "cam_image", "uyvy");
  if (source_bytes != (uint32_t)k_cam_frame_bytes) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t width  = (uint32_t)k_cam_image_width_px;
  const uint32_t height = (uint32_t)k_cam_image_height_px;
  for (uint32_t y = 0U; y < height; y += 1U) {
    for (uint32_t x = 0U; x < width; x += 2U) {
      const uint32_t source = ((y * width) + x) * 2U;
      const uint8_t  cb     = uyvy[source];
      const uint8_t  y0     = uyvy[source + 1U];
      const uint8_t  cr     = uyvy[source + 2U];
      const uint8_t  y1     = uyvy[source + 3U];
      cam_image_scatter(x, y, cam_image_ycbcr_to_rgb(y0, cb, cr));
      cam_image_scatter(x + 1U, y, cam_image_ycbcr_to_rgb(y1, cb, cr));
    }
  }
  RA8_RETURN_ON_ERROR(ra8_cache_dcache_clean_by_addr(g_cam_rgb_0, (uint32_t)k_cam_rgb_frame_bytes),
                      "cam_image",
                      "clean rgb0");
  RA8_RETURN_ON_ERROR(ra8_cache_dcache_clean_by_addr(g_cam_rgb_90, (uint32_t)k_cam_rgb_frame_bytes),
                      "cam_image",
                      "clean rgb90");
  RA8_RETURN_ON_ERROR(
    ra8_cache_dcache_clean_by_addr(g_cam_rgb_180, (uint32_t)k_cam_rgb_frame_bytes),
    "cam_image",
    "clean rgb180");
  return ra8_cache_dcache_clean_by_addr(g_cam_rgb_270, (uint32_t)k_cam_rgb_frame_bytes);
}

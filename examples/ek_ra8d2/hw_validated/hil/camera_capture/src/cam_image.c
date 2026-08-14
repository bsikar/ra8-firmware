/**
 * @file examples/ek_ra8d2/hw_validated/hil/camera_capture/src/cam_image.c
 * @brief UYVY-to-RGB888 conversion and 0/90/180/270 SDRAM image production.
 * @details Converts one module-owned packed VGA frame into four statically
 *          allocated, cache-aligned RGB888 SDRAM views and performs cache
 *          maintenance for debugger-side validation.
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

#include "ra8_cache.h"
#include "ra8_check.h"

/** @brief Cache-line alignment shared by the SDRAM buffers and cache API. */
typedef enum : uint32_t {
  k_cam_image_cache_line_bytes = 32U, /**< Cortex-M85 data-cache line size. */
} cam_image_align_t;

/** @brief BT.601 limited-range fixed-point YCbCr-to-RGB coefficients. */
typedef enum : int32_t {
  k_cam_yuv_luma_black = 16,  /**< Limited-range black luma code.       */
  k_cam_yuv_chroma_mid = 128, /**< Neutral limited-range chroma code.   */
  k_cam_yuv_luma_scale = 298, /**< BT.601 fixed-point luma coefficient. */
  k_cam_yuv_red_cr     = 409, /**< BT.601 red-from-Cr coefficient.      */
  k_cam_yuv_green_cb   = 100, /**< BT.601 green-from-Cb coefficient.    */
  k_cam_yuv_green_cr   = 208, /**< BT.601 green-from-Cr coefficient.    */
  k_cam_yuv_blue_cb    = 516, /**< BT.601 blue-from-Cb coefficient.     */
  k_cam_yuv_rounding   = 128, /**< Fixed-point rounding bias.           */
  k_cam_yuv_shift      = 8,   /**< Fixed-point fractional-bit count.    */
} cam_image_yuv_coeff_t;

/** @brief One RGB888 pixel used while scattering the four orientations. */
typedef struct {
  uint8_t red;   /**< Red RGB888 component.   */
  uint8_t green; /**< Green RGB888 component. */
  uint8_t blue;  /**< Blue RGB888 component.  */
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

/**
 * @brief Clamp a signed RGB component to one byte.
 * @details Saturates negative and over-range fixed-point conversion results.
 * @param[in] component Signed RGB channel value.
 * @return Saturated channel byte.
 * @retval 0 The component was negative.
 * @retval UINT8_MAX The component exceeded one byte.
 * @pre `component` is a completed fixed-point conversion result.
 * @pre No mutable image state is required.
 * @post The return value lies in the inclusive byte range.
 * @post No global storage is modified.
 * @note In-range values are converted without scaling.
 * @since 0.1.0
 */
static uint8_t cam_image_clamp(int32_t component)
{
  if (component < 0) {
    return 0U;
  }
  if (component > (int32_t)UINT8_MAX) {
    return UINT8_MAX;
  }
  return (uint8_t)component;
}

/**
 * @brief Convert one limited-range YCbCr pixel to RGB888.
 * @details Applies fixed-point BT.601-style luma and chroma coefficients.
 * @param[in] y Limited-range luma byte.
 * @param[in] cb Blue-difference chroma byte.
 * @param[in] cr Red-difference chroma byte.
 * @return Converted RGB pixel.
 * @retval cam_rgb_t Saturated red, green, and blue channels.
 * @pre Input components are sampled from one packed UYVY pair.
 * @pre Fixed-point coefficient constants retain their configured scale.
 * @post Each returned channel lies in the byte range.
 * @post Input values and global buffers remain unchanged.
 * @note Conversion uses integer arithmetic only.
 * @since 0.1.0
 */
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

/**
 * @brief Write one RGB pixel to all four rotated row-major destinations.
 * @details Computes the 0, 90, 180, and 270 degree row-major offsets once.
 * @param[in] x Source-frame horizontal coordinate.
 * @param[in] y Source-frame vertical coordinate.
 * @param[in] rgb Pixel value to scatter.
 * @pre `x` and `y` are inside the VGA source dimensions.
 * @pre All four SDRAM output arrays are writable.
 * @post Each orientation receives exactly one RGB pixel.
 * @post No output byte outside the four computed pixels is modified.
 * @note Rotation angles are clockwise.
 * @since 0.1.0
 */
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

/**
 * @brief Convert one complete UYVY frame into all four RGB orientations.
 *
 * @param[in] uyvy Complete VGA UYVY source frame.
 * @pre `uyvy` points to at least ::k_cam_uyvy_frame_bytes readable bytes.
 * @post All four statically allocated RGB buffers contain complete views.
 * @note Thread safety: not thread-safe; writes module-owned image buffers.
 * @since 0.1.0
 */
static void cam_image_convert(const uint8_t* uyvy)
{
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
}

/**
 * @brief Clean all generated RGB views from the data cache.
 *
 * @return Status of the first failed cache-clean operation.
 * @retval k_ra8_ok All four image ranges were cleaned.
 * @pre ::cam_image_convert populated every RGB view.
 * @post Successful completion makes every RGB view visible outside the CPU.
 * @note Thread safety: not thread-safe; operates on module-owned buffers.
 * @since 0.1.0
 */
static ra8_err_t cam_image_clean_outputs(void)
{
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

ra8_err_t cam_image_generate(const uint8_t* uyvy, uint32_t source_bytes)
{
  RA8_CHECK_NULL_PTR(uyvy, "cam_image", "uyvy");
  if (source_bytes != (uint32_t)k_cam_uyvy_frame_bytes) {
    return k_ra8_err_invalid_arg;
  }
  cam_image_convert(uyvy);
  return cam_image_clean_outputs();
}

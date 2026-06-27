/**
 * @file ra_rabook_gray4.c
 * @brief 4-bpp grayscale image transcode -- downscale + quantise + nibble-pack.
 * @since Version 0.1.0
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_rabook_gray4.h"

#include <stdint.h>
#include <string.h>

#include "ra_attributes.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

/* -------------------------------------------------------------------------- */
/* Private constants */
/* -------------------------------------------------------------------------- */

/**
 * @enum ra_gray4_fp_t
 * @brief Q16.16 fixed-point constants for the bilinear downscale kernel.
 * @details Q16.16 represents 1.0 as 65536; the 32-bit normalisation shift is 32
 *          because each bilinear weight is a 16-bit fractional (product of two
 *          16-bit values = 32 bits of normalisation needed after accumulation).
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_fp_shift      = 16U,      /**< Integer part starts at bit 16.           */
  k_fp_unit       = 0x10000U, /**< 1.0 in Q16.16 (65536).                   */
  k_fp_mask       = 0xFFFFU,  /**< Fractional-bits mask (low 16 bits).      */
  k_fp_norm_shift = 32U,      /**< Right-shift to normalise 64-bit product. */
} ra_gray4_fp_t;

/**
 * @enum ra_gray4_nib_t
 * @brief Nibble packing constants.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_nib_shift  = 4U, /**< Shift even pixel into the high nibble of a byte. */
  k_nib_lo_msk = 1U, /**< Bit 0: whether pixel index is odd (low nibble).  */
} ra_gray4_nib_t;

static const char* const s_tag = "ra_rabook_gray4";

/* -------------------------------------------------------------------------- */
/* Private helpers */
/* -------------------------------------------------------------------------- */

/**
 * @brief Sample one bilinear-interpolated pixel at fixed-point source (sx_fp, sy_fp).
 * @details Q16.16 coordinates: integer bits select the top-left corner, the
 *          fractional bits weight the four neighbours.  Clamps sx1/sy1 at the
 *          image edge so out-of-bounds samples repeat the last pixel row/column.
 */
RA_INTERNAL
static uint8_t s_bilinear_sample(const uint8_t* src,
                                 uint16_t       src_w,
                                 uint16_t       src_h,
                                 uint64_t       sx_fp,
                                 uint64_t       sy_fp)
{
  uint16_t sx0 = (uint16_t)(sx_fp >> k_fp_shift);
  uint16_t sy0 = (uint16_t)(sy_fp >> k_fp_shift);
  uint16_t sx1 = (uint16_t)((sx0 + 1U < (uint32_t)src_w) ? sx0 + 1U : src_w - 1U);
  uint16_t sy1 = (uint16_t)((sy0 + 1U < (uint32_t)src_h) ? sy0 + 1U : src_h - 1U);
  uint64_t fx  = sx_fp & (uint64_t)k_fp_mask;
  uint64_t fy  = sy_fp & (uint64_t)k_fp_mask;
  uint64_t ifx = (uint64_t)k_fp_unit - fx;
  uint64_t ify = (uint64_t)k_fp_unit - fy;

  uint64_t p00 = src[(uint32_t)sy0 * src_w + sx0];
  uint64_t p10 = src[(uint32_t)sy0 * src_w + sx1];
  uint64_t p01 = src[(uint32_t)sy1 * src_w + sx0];
  uint64_t p11 = src[(uint32_t)sy1 * src_w + sx1];

  uint64_t val = p00 * ifx * ify + p10 * fx * ify + p01 * ifx * fy + p11 * fx * fy;
  return (uint8_t)(val >> k_fp_norm_shift);
}

/* -------------------------------------------------------------------------- */
/* Public API */
/* -------------------------------------------------------------------------- */

void ra_rabook_gray4_output_dims(uint16_t  src_w,
                                 uint16_t  src_h,
                                 uint16_t  max_edge,
                                 uint16_t* out_w,
                                 uint16_t* out_h)
{
  if (out_w == nullptr || out_h == nullptr) {
    return;
  }

  if (src_w == 0U || src_h == 0U || max_edge == 0U) {
    *out_w = 0U;
    *out_h = 0U;
    return;
  }

  uint16_t longer = (src_w >= src_h) ? src_w : src_h;
  if (longer <= max_edge) {
    *out_w = src_w;
    *out_h = src_h;
    return;
  }

  uint32_t half_longer = (uint32_t)longer / 2U;
  *out_w               = (uint16_t)(((uint32_t)src_w * max_edge + half_longer) / longer);
  *out_h               = (uint16_t)(((uint32_t)src_h * max_edge + half_longer) / longer);

  if (*out_w == 0U) {
    *out_w = 1U;
  }
  if (*out_h == 0U) {
    *out_h = 1U;
  }
}

ra_err_t ra_rabook_gray4_downscale(const uint8_t* src,
                                   uint16_t       src_w,
                                   uint16_t       src_h,
                                   uint8_t*       dst,
                                   uint16_t       dst_w,
                                   uint16_t       dst_h)
{
  RA_CHECK_NULL_PTR(src, s_tag, "src");
  RA_CHECK_NULL_PTR(dst, s_tag, "dst");

  if (dst_w == 0U || dst_h == 0U) {
    ra_log_error(s_tag, "dst_w or dst_h is zero");
    return k_ra_err_invalid_arg;
  }

  if (src_w == 0U || src_h == 0U) {
    (void)memset(dst, 0, (size_t)dst_w * (size_t)dst_h);
    return k_ra_ok;
  }

  uint64_t x_step = ((uint64_t)src_w << k_fp_shift) / dst_w;
  uint64_t y_step = ((uint64_t)src_h << k_fp_shift) / dst_h;

  RA_BOUNDED_LOOP(dst_h)
  for (uint16_t dy = 0U; dy < dst_h; dy++) {
    uint64_t sy_fp = (uint64_t)dy * y_step;

    RA_BOUNDED_LOOP(dst_w)
    for (uint16_t dx = 0U; dx < dst_w; dx++) {
      uint64_t sx_fp                 = (uint64_t)dx * x_step;
      dst[(uint32_t)dy * dst_w + dx] = s_bilinear_sample(src, src_w, src_h, sx_fp, sy_fp);
    }
  }

  return k_ra_ok;
}

ra_err_t ra_rabook_gray4_encode(const uint8_t* gray_pixels,
                                uint16_t       w,
                                uint16_t       h,
                                uint8_t*       out,
                                uint32_t       out_cap,
                                uint32_t*      out_size)
{
  RA_CHECK_NULL_PTR(gray_pixels, s_tag, "gray_pixels");
  RA_CHECK_NULL_PTR(out, s_tag, "out");
  RA_CHECK_NULL_PTR(out_size, s_tag, "out_size");

  uint32_t n_pixels = (uint32_t)w * h;
  uint32_t n_bytes  = (n_pixels + 1U) / k_ra_rabook_gray4_nib_per_byte;

  if (n_pixels == 0U) {
    *out_size = 0U;
    return k_ra_ok;
  }

  if (out_cap < n_bytes) {
    ra_log_error(s_tag, "output buffer too small");
    return k_ra_err_no_mem;
  }

  (void)memset(out, 0, n_bytes);

  RA_BOUNDED_LOOP(n_pixels)
  for (uint32_t i = 0U; i < n_pixels; i++) {
    uint8_t nib = (uint8_t)((gray_pixels[i] + (uint8_t)k_ra_rabook_gray4_round_half) /
                            (uint8_t)k_ra_rabook_gray4_quant_div);
    if (nib > (uint8_t)k_ra_rabook_gray4_nib_max) {
      nib = (uint8_t)k_ra_rabook_gray4_nib_max;
    }

    uint32_t byte_idx = i / k_ra_rabook_gray4_nib_per_byte;
    if ((i & (uint32_t)k_nib_lo_msk) == 0U) {
      out[byte_idx] = (uint8_t)(nib << k_nib_shift);
    } else {
      out[byte_idx] |= nib;
    }
  }

  *out_size = n_bytes;
  return k_ra_ok;
}

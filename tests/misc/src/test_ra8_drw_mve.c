/**
 * @file test_ra8_drw_mve.c
 * @brief Host unit and parity tests for DRW sub-rect HUM contract and MVE blits.
 *
 * @details
 * Verifies:
 *   - HUM Ch 62.2.16 p 3700 ``TEXMASK[31:11]`` / ``TEXMASK[10:0]`` encoding
 *   - HUM Ch 62.6.3.2 p 3731 V-limiter ``TEXPITCH`` scaling & sub-rect offsets
 *   - HUM Ch 62.6.4 Table 62.8 Copy colorization (``COLOR1=0``, ``COLOR2=~0``)
 *   - RGB888, Gray8, and RGBA8888 -> RGB565 conversion parity across edge lengths
 *   - Viewport clipping, multi-strip staging, and zero-copy RGB565 band blits
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_drw.h"
#include "ra8_drw_mve.h"
#include "ra8_drw_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/** @brief Test constants matching fake-MMIO SDRAM aperture and panel geometry. */
typedef enum : uint32_t {
  k_t_fb_addr     = 0x68000000UL, /**< Framebuffer SDRAM test address. */
  k_t_tex_addr    = 0x68100000UL, /**< Texture SDRAM test address.     */
  k_t_view_w      = 1024U,        /**< Longstrip panel width.          */
  k_t_view_h      = 600U,         /**< Longstrip panel height.         */
  k_t_band_h      = 280U,         /**< Longstrip band height.          */
  k_t_small_w     = 32U,          /**< Small test strip width.         */
  k_t_small_h     = 24U,          /**< Small test strip height.        */
  k_t_fill_col    = 0xFF112233UL, /**< Non-zero fill color to clobber. */
  k_t_budget      = 64U,          /**< Host wait_idle poll budget.     */
  k_t_max_test_px = 80U,          /**< Max 1D conversion test buffer.  */
} drw_mve_test_const_t;

/** @brief Reset fake MMIO registers and MSTP state for one test case. */
RA8_INTERNAL static void internal_prep_drw(uint16_t pitch_px)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  const ra8_drw_config_t cfg = {
    .framebuffer_addr       = (uintptr_t)k_t_fb_addr,
    .pitch_px               = pitch_px,
    .format                 = k_ra8_drw_writefmt_rgb565,
    .enable_caches          = false,
    .enable_buffered_writes = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_init(&cfg));
}

/** @brief Scalar ground-truth RGB565 packer matching hardware and ra8_gfx. */
RA8_INTERNAL static uint16_t internal_ref_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
  return (uint16_t)(((uint32_t)(r >> 3U) << 11U) | ((uint32_t)(g >> 2U) << 5U) |
                    (uint32_t)(b >> 3U));
}

/** @brief Verify HUM Ch 62.2.16 TEXMASK[31:11] packing and validation limits. */
RA8_INTERNAL static void internal_test_texmask_hum(void)
{
  TEST_BEGIN("drw_mve: TEXMASK [31:11] packing and bounds");
  internal_prep_drw((uint16_t)k_t_view_w);

  ra8_drw_texture_t tex = {
    .base_addr = (uintptr_t)k_t_tex_addr,
    .pitch_px  = (uint16_t)k_t_view_w,
    .u_mask    = (uint16_t)(k_t_view_w - 1U),
    .v_mask    = (uint32_t)k_t_view_w * ((uint32_t)k_t_band_h - 1U),
    .format    = k_ra8_drw_readfmt_rgb565,
    .clamp_x   = true,
    .clamp_y   = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_set_texture(&tex));
  const uint32_t expected = (tex.v_mask << k_ra8_drw_texmask_vmask_pos) | (uint32_t)tex.u_mask;
  TEST_ASSERT_EQ(expected, *ra8_drw_reg32(k_ra8_drw_off_texmask));

  tex.pitch_px = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_set_texture(&tex));
  tex.pitch_px = (uint16_t)k_t_view_w;
  tex.u_mask   = (uint16_t)(k_ra8_drw_texmask_umask_max + 1U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_set_texture(&tex));
  tex.u_mask = (uint16_t)k_t_view_w - 1U;
  tex.v_mask = (uint32_t)k_ra8_drw_texmask_vmask_max + 1UL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_set_texture(&tex));
  TEST_END("drw_mve: TEXMASK [31:11] packing and bounds");
}

/** @brief Verify sub-rect U/V limiter programming and Copy-mode COLOR1/2 reset. */
RA8_INTERNAL static void internal_test_subrect_hum_registers(void)
{
  TEST_BEGIN("drw_mve: blit_textured_subrect HUM registers and Copy reset");
  internal_prep_drw((uint16_t)k_t_view_w);

  const ra8_drw_rect_t fill = {
    .x              = 0,
    .y              = 0,
    .width_px       = 4U,
    .height_px      = 4U,
    .color_argb8888 = k_t_fill_col,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_fill_rect(&fill));

  const ra8_drw_rect_t rect = {
    .x              = 10,
    .y              = 20,
    .width_px       = 64U,
    .height_px      = 32U,
    .color_argb8888 = k_t_fill_col,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_drw_blit_textured_subrect(&rect, 0U, 0U));

  const ra8_drw_texture_t tex = {
    .base_addr = (uintptr_t)k_t_tex_addr,
    .pitch_px  = 256U,
    .u_mask    = 255U,
    .v_mask    = 256UL * 127UL,
    .format    = k_ra8_drw_readfmt_rgb565,
    .clamp_x   = true,
    .clamp_y   = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_set_texture(&tex));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_drw_blit_textured_subrect(&rect, (uint16_t)(k_ra8_drw_texmask_umask_max + 1U), 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_drw_blit_textured_subrect(&rect, 0U, (uint16_t)k_ra8_drw_max_height_px));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_blit_textured_subrect(&rect, 12U, 45U));
  TEST_ASSERT_EQ(12UL * k_ra8_drw_subpixel_unit, *ra8_drw_reg32(k_ra8_drw_off_lustart));
  TEST_ASSERT_EQ((uint32_t)k_ra8_drw_subpixel_unit, *ra8_drw_reg32(k_ra8_drw_off_luxadd));
  TEST_ASSERT_EQ(0UL, *ra8_drw_reg32(k_ra8_drw_off_luyadd));
  TEST_ASSERT_EQ(45UL * 256UL, *ra8_drw_reg32(k_ra8_drw_off_lvstarti));
  TEST_ASSERT_EQ(256UL, *ra8_drw_reg32(k_ra8_drw_off_lvyaddi));
  TEST_ASSERT_EQ(k_ra8_drw_colorize_copy_color1, *ra8_drw_reg32(k_ra8_drw_off_color1));
  TEST_ASSERT_EQ(k_ra8_drw_colorize_copy_color2, *ra8_drw_reg32(k_ra8_drw_off_color2));
  TEST_END("drw_mve: blit_textured_subrect HUM registers and Copy reset");
}

/** @brief Verify 1D conversion parity across lengths straddling 8-pixel beats. */
RA8_INTERNAL static void internal_test_1d_pixel_conversions(void)
{
  TEST_BEGIN("drw_mve: 1D rgb888/gray8/rgba8888 to rgb565 parity");
  static const uint32_t k_lens[] = {0U, 1U, 7U, 8U, 9U, 15U, 16U, 17U, 65U};
  uint8_t               src[k_t_max_test_px * 4U];
  uint16_t              dst[k_t_max_test_px];

  for (uint32_t i = 0U; i < (uint32_t)sizeof(src); ++i) {
    src[i] = (uint8_t)((i * 37U + 13U) & 0xFFU);
  }
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_mve_rgb888_to_rgb565(nullptr, dst, 4U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_mve_rgb888_to_rgb565(src, nullptr, 4U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_mve_gray8_to_rgb565(nullptr, dst, 4U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_mve_gray8_to_rgb565(src, nullptr, 4U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_mve_rgba8888_to_rgb565(nullptr, dst, 4U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_mve_rgba8888_to_rgb565(src, nullptr, 4U));

  for (uint32_t li = 0U; li < (uint32_t)(sizeof(k_lens) / sizeof(k_lens[0])); ++li) {
    const uint32_t n = k_lens[li];
    TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_mve_rgb888_to_rgb565(src, dst, n));
    for (uint32_t i = 0U; i < n; ++i) {
      TEST_ASSERT_EQ(internal_ref_rgb565(src[i * 3U], src[(i * 3U) + 1U], src[(i * 3U) + 2U]),
                     dst[i]);
    }
    TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_mve_gray8_to_rgb565(src, dst, n));
    for (uint32_t i = 0U; i < n; ++i) {
      TEST_ASSERT_EQ(internal_ref_rgb565(src[i], src[i], src[i]), dst[i]);
    }
    TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_mve_rgba8888_to_rgb565(src, dst, n));
    for (uint32_t i = 0U; i < n; ++i) {
      TEST_ASSERT_EQ(internal_ref_rgb565(src[i * 4U], src[(i * 4U) + 1U], src[(i * 4U) + 2U]),
                     dst[i]);
    }
  }
  TEST_END("drw_mve: 1D rgb888/gray8/rgba8888 to rgb565 parity");
}

/** @brief Verify 2D strided rectangle conversion across bpp 1, 2, 3, 4 and errors. */
RA8_INTERNAL static void internal_test_2d_rect_conversion(void)
{
  TEST_BEGIN("drw_mve: 2D strided sub-rect convert across bpp 1..4");
  uint8_t  src[24U * 16U * 4U];
  uint16_t dst[16U * 16U];
  for (uint32_t i = 0U; i < (uint32_t)sizeof(src); ++i) {
    src[i] = (uint8_t)((i * 19U + 5U) & 0xFFU);
  }
  const uint8_t bpps[] = {1U, 2U, 3U, 4U};
  for (uint32_t bi = 0U; bi < 4U; ++bi) {
    const uint8_t                 bpp  = bpps[bi];
    const ra8_drw_mve_rect_conv_t conv = {
      .src           = src,
      .dst           = dst,
      .src_stride_px = 24U,
      .dst_stride_px = 16U,
      .src_x         = 5U,
      .src_y         = 3U,
      .width_px      = 13U,
      .height_px     = 7U,
      .src_bpp       = bpp,
    };
    TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_mve_convert_rect_to_rgb565(&conv));
    for (uint16_t r = 0U; r < conv.height_px; ++r) {
      for (uint16_t c = 0U; c < conv.width_px; ++c) {
        const size_t   soff = ((((size_t)3U + r) * 24U) + (5U + c)) * (size_t)bpp;
        const uint16_t got  = dst[((size_t)r * 16U) + c];
        if (bpp == 1U) {
          TEST_ASSERT_EQ(internal_ref_rgb565(src[soff], src[soff], src[soff]), got);
        } else if (bpp == 2U) {
          TEST_ASSERT_EQ((uint16_t)src[soff] | ((uint16_t)src[soff + 1U] << 8U), got);
        } else {
          TEST_ASSERT_EQ(internal_ref_rgb565(src[soff], src[soff + 1U], src[soff + 2U]), got);
        }
      }
    }
  }
  ra8_drw_mve_rect_conv_t bad = {.src           = src,
                                 .dst           = dst,
                                 .src_stride_px = 10U,
                                 .dst_stride_px = 10U,
                                 .src_x         = 0U,
                                 .src_y         = 0U,
                                 .width_px      = 11U,
                                 .height_px     = 2U,
                                 .src_bpp       = 3U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_mve_convert_rect_to_rgb565(&bad));
  bad.width_px = 4U;
  bad.src_bpp  = 5U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_mve_convert_rect_to_rgb565(&bad));
  TEST_END("drw_mve: 2D strided sub-rect convert across bpp 1..4");
}

/** @brief Verify band clipping, chunked staging, and zero-copy rgb565 DRW blit. */
RA8_INTERNAL static void internal_test_band_blit_clipping_and_staging(void)
{
  TEST_BEGIN("drw_mve: band blit clipping, chunked staging, zero-copy rgb565");
  internal_prep_drw(64U);

  uint8_t  src[(size_t)k_t_small_w * (size_t)k_t_small_h * 3U];
  uint16_t stage[k_t_small_w * 4U];
  uint16_t fb[64U * 32U];
  for (uint32_t i = 0U; i < (uint32_t)sizeof(src); ++i) {
    src[i] = (uint8_t)((i * 41U + 7U) & 0xFFU);
  }
  (void)memset(fb, 0, sizeof(fb));

  /* Completely off-screen: returns OK without triggering ORIGIN. */
  *ra8_drw_reg32(k_ra8_drw_off_origin) = 0UL;
  ra8_drw_mve_band_blit_t job          = {
    .src_pixels     = src,
    .src_w          = (uint16_t)k_t_small_w,
    .src_h          = (uint16_t)k_t_small_h,
    .src_bpp        = (uint8_t)k_ra8_drw_px_bpp_rgb888,
    .dst_x          = 0,
    .dst_y          = -40,
    .view_w         = 64U,
    .view_h         = 32U,
    .stage_565      = stage,
    .stage_cap_px   = (uint32_t)(k_t_small_w * 4U),
    .stage_tex_addr = (uint32_t)k_t_tex_addr,
    .fb_ptr         = fb,
    .fb_pitch_px    = 64U,
    .poll_budget    = (uint32_t)k_t_budget,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_mve_blit_band(&job));
  TEST_ASSERT_EQ(0UL, *ra8_drw_reg32(k_ra8_drw_off_origin));

  /* Partially clipped top band (dst_y=-14 -> visible h=10 in 3 chunk strips of <=4 rows). */
  job.dst_x = 8;
  job.dst_y = -14;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_mve_blit_band(&job));
  TEST_ASSERT_EQ((uint32_t)k_t_tex_addr, *ra8_drw_reg32(k_ra8_drw_off_texorigin));
  TEST_ASSERT_EQ((uint32_t)k_t_small_w, *ra8_drw_reg32(k_ra8_drw_off_lvyaddi));
  /* TEXTUREENABLE must be cleared after blit completes. */
  TEST_ASSERT_EQ(0UL, *ra8_drw_reg32(k_ra8_drw_off_control2) & k_ra8_drw_control2_textureenable);

  /* Zero-copy direct RGB565 path when stage_565 == nullptr. */
  job.src_bpp      = (uint8_t)k_ra8_drw_px_bpp_rgb565;
  job.stage_565    = nullptr;
  job.src_tex_addr = (uint32_t)k_t_tex_addr;
  job.dst_x        = -4;
  job.dst_y        = -5;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_mve_blit_band(&job));
  TEST_ASSERT_EQ(4UL * k_ra8_drw_subpixel_unit, *ra8_drw_reg32(k_ra8_drw_off_lustart));
  TEST_ASSERT_EQ(5UL * k_t_small_w, *ra8_drw_reg32(k_ra8_drw_off_lvstarti));
  TEST_ASSERT_EQ(false, ra8_drw_mve_is_hardware_enabled());
  TEST_END("drw_mve: band blit clipping, chunked staging, zero-copy rgb565");
}

/**
 * @test internal_test_mcdc_drw_mve_band_guards
 * @par MC/DC:
 * Decision in `ra8_drw_mve_blit_band`:
 *   `(src_bpp == rgb565) && (stage_565 == nullptr)` (2 conditions C1, C2).
 * - V1: src_bpp=rgb565, stage=nullptr -> T && T -> T (zero-copy branch)
 * - V2: src_bpp=rgb888, stage=valid   -> F (short-circuit) -> F (staged branch)
 * - V3: src_bpp=rgb565, stage=valid   -> T && F -> F (staged branch)
 * @brief Verify MC/DC vectors on zero-copy vs staged band-blit selection.
 */
RA8_INTERNAL static void internal_test_mcdc_drw_mve_band_guards(void)
{
  TEST_BEGIN("drw_mve MC/DC: zero-copy vs staged selection");
  internal_prep_drw(32U);
  uint8_t                 src[16U * 4U * 3U] = {};
  uint16_t                stage[16U * 4U]    = {};
  ra8_drw_mve_band_blit_t job                = {
    .src_pixels     = src,
    .src_w          = 16U,
    .src_h          = 4U,
    .src_bpp        = (uint8_t)k_ra8_drw_px_bpp_rgb565,
    .dst_x          = 0,
    .dst_y          = 0,
    .view_w         = 32U,
    .view_h         = 16U,
    .stage_565      = nullptr,
    .stage_cap_px   = 64U,
    .stage_tex_addr = (uint32_t)k_t_tex_addr,
    .src_tex_addr   = (uint32_t)k_t_fb_addr + 0x1000UL,
    .poll_budget    = (uint32_t)k_t_budget,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_mve_blit_band(&job));
  TEST_ASSERT_EQ((uint32_t)k_t_fb_addr + 0x1000UL, *ra8_drw_reg32(k_ra8_drw_off_texorigin));

  job.src_bpp   = (uint8_t)k_ra8_drw_px_bpp_rgb888;
  job.stage_565 = stage;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_mve_blit_band(&job));
  TEST_ASSERT_EQ((uint32_t)k_t_tex_addr, *ra8_drw_reg32(k_ra8_drw_off_texorigin));

  job.src_bpp   = (uint8_t)k_ra8_drw_px_bpp_rgb565;
  job.stage_565 = stage;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_mve_blit_band(&job));
  TEST_ASSERT_EQ((uint32_t)k_t_tex_addr, *ra8_drw_reg32(k_ra8_drw_off_texorigin));

  job.stage_cap_px = 4U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_drw_mve_blit_band(&job));
  job.src_bpp = 5U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_mve_blit_band(&job));
  TEST_END("drw_mve MC/DC: zero-copy vs staged selection");
}

int main(void)
{
  internal_test_texmask_hum();
  internal_test_subrect_hum_registers();
  internal_test_1d_pixel_conversions();
  internal_test_2d_rect_conversion();
  internal_test_band_blit_clipping_and_staging();
  internal_test_mcdc_drw_mve_band_guards();
  return 0;
}

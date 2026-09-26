/**
 * @file test_ra8_gfx_surface.c
 * @brief Unit tests for the ra8_gfx surface bind (#737) and its teardown.
 *
 * @details
 * The positional ra8_gfx_init() cannot carry a row pitch, so a padded
 * framebuffer had to be described as if it were densely packed. These tests
 * bind through ra8_gfx_init_surface() instead and assert the two properties
 * that only a stride-carrying bind can have: row y starts at
 * `pixels + y * stride_bytes`, and the padding bytes past the last pixel of a
 * row are never written. The teardown half (ra8_gfx_deinit) is checked for the
 * lifecycle symmetry the seam was missing.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "unity_minimal.h"

/**
 * @enum surf_fixture_t
 * @brief Fixture geometry for the padded-surface framebuffer.
 */
typedef enum : uint16_t {
  k_surf_w      = 8,      /**< Visible width in pixels.      */
  k_surf_h      = 4,      /**< Visible height in pixels.     */
  k_surf_pad_px = 3,      /**< Padding pixels past each row. */
  k_surf_bpp    = 2,      /**< RGB565 bytes per pixel.       */
  k_surf_packed = 16,     /**< One densely packed row.       */
  k_surf_stride = 22,     /**< Packed row plus the padding.  */
  k_surf_bytes  = 88,     /**< k_surf_h rows of pitch.       */
  k_surf_guard  = 0xA5U,  /**< Fill byte no draw may leave.  */
} surf_fixture_t;

/** @brief Padded 8x4 RGB565 buffer: 16 payload bytes + 6 pad bytes per row. */
static uint8_t s_padded[k_surf_bytes];

/**
 * @brief Fill the padded buffer with the guard byte and bind it as a surface.
 *
 * @return ra8_err_t Result of the bind, for the caller to assert on.
 */
static ra8_err_t bind_padded(void)
{
  (void)memset(s_padded, (int)k_surf_guard, sizeof(s_padded));
  const ra8_gfx_surface_t surface = {
    .pixels       = s_padded,
    .w            = k_surf_w,
    .h            = k_surf_h,
    .stride_bytes = k_surf_stride,
    .fmt          = k_ra8_gfx_format_rgb565,
  };
  return ra8_gfx_init_surface(&surface);
}

/**
 * @par MC/DC:
 * (no compound decisions -- exercises the happy-path bind contract)
 *
 * @brief A clear() over a padded surface writes payload bytes and no padding.
 */
static void test_surface_clear_skips_row_padding(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, bind_padded());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear(0x00000000U));

  for (uint32_t row = 0U; row < (uint32_t)k_surf_h; ++row) {
    const uint8_t* const base = &s_padded[row * (uint32_t)k_surf_stride];
    for (uint32_t i = 0U; i < (uint32_t)k_surf_packed; ++i) {
      TEST_ASSERT_EQ(0x00U, base[i]);
    }
    for (uint32_t i = (uint32_t)k_surf_packed; i < (uint32_t)k_surf_stride; ++i) {
      TEST_ASSERT_EQ((uint8_t)k_surf_guard, base[i]);
    }
  }
}

/**
 * @par MC/DC:
 * (no compound decisions -- pixel addressing over a padded pitch)
 *
 * @brief Pixel (0, y) lands at `pixels + y * stride_bytes`, not `y * w * bpp`.
 *
 * @details
 * With a packed-row assumption row 1 would start at byte 16; the surface bind
 * has to put it at byte 22. Writing white at (0,1) and (0,2) and reading the
 * raw bytes is the whole finding of #737 in one assertion.
 */
static void test_surface_row_origin_follows_stride(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, bind_padded());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_pixel(0, 1, 0x00FFFFFFU));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_pixel(0, 2, 0x00FFFFFFU));

  TEST_ASSERT_EQ(0xFFU, s_padded[(uint32_t)k_surf_stride]);
  TEST_ASSERT_EQ(0xFFU, s_padded[(uint32_t)k_surf_stride + 1U]);
  TEST_ASSERT_EQ(0xFFU, s_padded[2U * (uint32_t)k_surf_stride]);
  TEST_ASSERT_EQ(0xFFU, s_padded[(2U * (uint32_t)k_surf_stride) + 1U]);
  /* The byte a packed-row rasteriser would have written instead. */
  TEST_ASSERT_EQ((uint8_t)k_surf_guard, s_padded[(uint32_t)k_surf_packed]);
}

/**
 * @par MC/DC:
 * (no compound decisions -- a filled rect over a padded pitch)
 *
 * @brief A full-width filled rect stops at the visible edge of every row.
 */
static void test_surface_rect_stops_at_visible_edge(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, bind_padded());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_rect(0, 0, (int32_t)k_surf_w, (int32_t)k_surf_h, 0x00FFFFFFU, true));

  for (uint32_t row = 0U; row < (uint32_t)k_surf_h; ++row) {
    const uint8_t* const base = &s_padded[row * (uint32_t)k_surf_stride];
    TEST_ASSERT_EQ(0xFFU, base[(uint32_t)k_surf_packed - 1U]);
    TEST_ASSERT_EQ((uint8_t)k_surf_guard, base[(uint32_t)k_surf_packed]);
  }
}

/**
 * @par MC/DC:
 * (decision `s == nullptr` and `s->pixels == nullptr` each taken both ways
 * across the two null cases below)
 *
 * @brief A null descriptor and a null pixel pointer are both rejected.
 */
static void test_surface_null_rejected(void)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_init_surface(nullptr));

  const ra8_gfx_surface_t no_pixels = {
    .pixels       = nullptr,
    .w            = k_surf_w,
    .h            = k_surf_h,
    .stride_bytes = k_surf_stride,
    .fmt          = k_ra8_gfx_format_rgb565,
  };
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_init_surface(&no_pixels));
}

/**
 * @par MC/DC:
 * (decision `s->stride_bytes < packed_row` taken both ways: one byte short
 * rejects, exactly packed accepts)
 *
 * @brief A pitch narrower than one packed row is rejected; exactly packed is not.
 */
static void test_surface_stride_below_packed_row_rejected(void)
{
  ra8_gfx_surface_t surface = {
    .pixels       = s_padded,
    .w            = k_surf_w,
    .h            = k_surf_h,
    .stride_bytes = (uint32_t)k_surf_packed - 1U,
    .fmt          = k_ra8_gfx_format_rgb565,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gfx_init_surface(&surface));

  surface.stride_bytes = (uint32_t)k_surf_packed;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init_surface(&surface));
}

/**
 * @par MC/DC:
 * (decisions on width range, height range and format validity each taken
 * both ways against the accepted fixture)
 *
 * @brief Out-of-range dimensions and an unsupported format are rejected.
 */
static void test_surface_dimension_and_format_rejected(void)
{
  ra8_gfx_surface_t surface = {
    .pixels       = s_padded,
    .w            = 0,
    .h            = k_surf_h,
    .stride_bytes = k_surf_stride,
    .fmt          = k_ra8_gfx_format_rgb565,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gfx_init_surface(&surface));

  surface.w = k_surf_w;
  surface.h = 0;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gfx_init_surface(&surface));

  surface.h   = k_surf_h;
  surface.fmt = (ra8_gfx_format_t)0;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gfx_init_surface(&surface));
}

/**
 * @par MC/DC:
 * (decision `!initialized` in ra8_gfx_deinit taken both ways: bound then
 * unbound)
 *
 * @brief deinit drops the binding; draws then fail until a fresh bind.
 */
static void test_deinit_unbinds_and_blocks_draws(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, bind_padded());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_deinit());

  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_gfx_clear(0x00000000U));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_gfx_pixel(0, 0, 0x00FFFFFFU));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_gfx_reset_clip());
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_gfx_deinit());

  /* The buffer the caller lent is untouched by the teardown itself. */
  TEST_ASSERT_EQ((uint8_t)k_surf_guard, s_padded[0]);

  TEST_ASSERT_EQ(k_ra8_ok, bind_padded());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear(0x00000000U));
  TEST_ASSERT_EQ(0x00U, s_padded[0]);
}

/**
 * @par MC/DC:
 * (no compound decisions -- equivalence of the two bind forms)
 *
 * @brief A packed surface bind draws byte-for-byte like the positional bind.
 */
static void test_surface_matches_positional_when_packed(void)
{
  static uint8_t expected[k_surf_h * k_surf_packed];
  static uint8_t actual[k_surf_h * k_surf_packed];

  (void)memset(expected, 0, sizeof(expected));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(expected, k_surf_w, k_surf_h, k_ra8_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_rect(1, 1, 5, 2, 0x00FF0000U, true));

  (void)memset(actual, 0, sizeof(actual));
  const ra8_gfx_surface_t surface = {
    .pixels       = actual,
    .w            = k_surf_w,
    .h            = k_surf_h,
    .stride_bytes = (uint32_t)k_surf_packed,
    .fmt          = k_ra8_gfx_format_rgb565,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init_surface(&surface));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_rect(1, 1, 5, 2, 0x00FF0000U, true));

  TEST_ASSERT_EQ(0, memcmp(expected, actual, sizeof(expected)));
}

/** @brief Roster of every test case in this file, run in order by main(). */
static void (*const s_test_roster[])(void) = {
  test_surface_clear_skips_row_padding,
  test_surface_row_origin_follows_stride,
  test_surface_rect_stops_at_visible_edge,
  test_surface_null_rejected,
  test_surface_stride_below_packed_row_rejected,
  test_surface_dimension_and_format_rejected,
  test_deinit_unbinds_and_blocks_draws,
  test_surface_matches_positional_when_packed,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}

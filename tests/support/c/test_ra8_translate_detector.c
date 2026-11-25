/**
 * @file test_ra8_translate_detector.c
 * @brief Host C23 ABI acceptance tests for `libs/ra8_translate` detector post-processing.
 *
 * Exercises `ra8_translate_det_*` C exports across int8 and float detector maps:
 * connected components, furigana suppression, bubble grouping, reading order, and
 * fail-closed error guards.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ra8_translate.h"
#include "unity_minimal.h"

static void
paint_rect_f32(float* map, uint16_t w, uint16_t x0, uint16_t y0, uint16_t rw, uint16_t rh, float v)
{
  for (uint16_t y = y0; y < (uint16_t)(y0 + rh); ++y) {
    for (uint16_t x = x0; x < (uint16_t)(x0 + rw); ++x) {
      map[(size_t)y * w + x] = v;
    }
  }
}

static void
paint_rect_i8(int8_t* map, uint16_t w, uint16_t x0, uint16_t y0, uint16_t rw, uint16_t rh, int8_t v)
{
  for (uint16_t y = y0; y < (uint16_t)(y0 + rh); ++y) {
    for (uint16_t x = x0; x < (uint16_t)(x0 + rw); ++x) {
      map[(size_t)y * w + x] = v;
    }
  }
}

static void test_detector_manga_furigana_and_rtl(void)
{
  enum : uint16_t { k_w = 32U, k_h = 32U };
  float map[k_w * k_h] = {};

  /* Right bubble (x=20..26, y=3..19): 2 kanji columns + 1 furigana column between/beside them. */
  paint_rect_f32(map, k_w, 24U, 3U, 3U, 15U, 0.90F);
  paint_rect_f32(map, k_w, 28U, 4U, 1U, 8U, 0.88F); /* Furigana right of x=24..27 */
  paint_rect_f32(map, k_w, 20U, 3U, 3U, 15U, 0.90F);

  /* Left bubble (x=4..10, y=4..14) */
  paint_rect_f32(map, k_w, 4U, 4U, 5U, 10U, 0.85F);

  /* Bottom-right bubble (x=18..26, y=22..28) */
  paint_rect_f32(map, k_w, 18U, 22U, 8U, 6U, 0.95F);

  size_t work_bytes = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_translate_det_work_bytes(k_w, 16U, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_det_work_bytes(k_w, 16U, &work_bytes));

  uint8_t work[8192] = {};
  TEST_ASSERT(work_bytes <= sizeof(work));

  const ra8_translate_det_cfg_t cfg = {
    .map_width              = k_w,
    .map_height             = k_h,
    .row_stride             = 0U,
    .elem_stride            = 0U,
    .page_width             = 320U,
    .page_height            = 320U,
    .text_thresh_bps        = k_ra8_translate_det_def_txt_bps,
    .bubble_thresh_bps      = k_ra8_translate_det_def_bub_bps,
    .min_comp_area          = 4U,
    .box_pad_pct            = 0U,
    .group_gap_pct          = k_ra8_translate_det_def_gap_pct,
    .furigana_max_ratio_pct = k_ra8_translate_det_def_furi_pct,
    .furigana_max_gap_pct   = k_ra8_translate_det_def_fgap_pct,
    .reading_order          = k_ra8_translate_det_order_manga_rtl,
    .flags                  = k_ra8_translate_det_flag_drop_furigana,
    .max_components         = 16U,
    .reserved               = {0U, 0U},
  };
  ra8_translate_det_line_t    lines[16]   = {};
  ra8_translate_det_bubble_t  bubbles[16] = {};
  ra8_translate_det_summary_t summary     = {};

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_det_post_f32(map,
                                            k_w * k_h,
                                            nullptr,
                                            0U,
                                            &cfg,
                                            work,
                                            work_bytes,
                                            lines,
                                            16U,
                                            bubbles,
                                            16U,
                                            &summary));
  TEST_ASSERT_EQ(5U, summary.raw_components);
  TEST_ASSERT_EQ(1U, summary.dropped_furigana);
  TEST_ASSERT_EQ(4U, summary.line_count);
  TEST_ASSERT_EQ(3U, summary.bubble_count);

  /* Manga RTL puts Top-Right -> Top-Left -> Bottom-Right. */
  TEST_ASSERT_EQ(200U, bubbles[0].x);
  TEST_ASSERT_EQ(2U, bubbles[0].line_count);
  TEST_ASSERT_EQ(240U, lines[bubbles[0].first_line].x);
  TEST_ASSERT_EQ(200U, lines[bubbles[0].first_line + 1U].x);
  TEST_ASSERT_EQ(40U, bubbles[1].x);
  TEST_ASSERT_EQ(180U, bubbles[2].x);
}

static void test_detector_webtoon_i8_and_error_guards(void)
{
  enum : uint16_t { k_w = 24U, k_h = 24U };
  int8_t txt_i8[k_w * k_h];
  memset(txt_i8, -100, sizeof(txt_i8));

  /* Webtoon horizontal top (y=2..5, x=4..16) & bottom (y=14..18, x=2..20) */
  paint_rect_i8(txt_i8, k_w, 4U, 2U, 12U, 3U, 80);
  paint_rect_i8(txt_i8, k_w, 2U, 14U, 18U, 4U, 90);

  size_t work_bytes = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_det_work_bytes(k_w, 8U, &work_bytes));
  uint8_t work[8192] = {};

  ra8_translate_det_cfg_t cfg = {
    .map_width              = k_w,
    .map_height             = k_h,
    .page_width             = 240U,
    .page_height            = 240U,
    .text_thresh_bps        = k_ra8_translate_det_def_txt_bps,
    .bubble_thresh_bps      = k_ra8_translate_det_def_bub_bps,
    .min_comp_area          = 4U,
    .group_gap_pct          = 100U,
    .furigana_max_ratio_pct = 55U,
    .furigana_max_gap_pct   = 120U,
    .reading_order          = k_ra8_translate_det_order_webtoon_ttb,
    .flags                  = 0U,
    .max_components         = 8U,
  };
  ra8_translate_det_line_t    lines[8]   = {};
  ra8_translate_det_bubble_t  bubbles[8] = {};
  ra8_translate_det_summary_t summary    = {.line_count = 42U};

  /* Unmatched bubble_len != 0 when bubble_map == nullptr must fail without mutating outputs. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_translate_det_post_i8(txt_i8,
                                           sizeof(txt_i8),
                                           0.005F,
                                           -100,
                                           nullptr,
                                           10U,
                                           0.005F,
                                           -100,
                                           &cfg,
                                           work,
                                           work_bytes,
                                           lines,
                                           8U,
                                           bubbles,
                                           8U,
                                           &summary));
  TEST_ASSERT_EQ(42U, summary.line_count);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_det_post_i8(txt_i8,
                                           sizeof(txt_i8),
                                           0.005F,
                                           -100,
                                           nullptr,
                                           0U,
                                           0.005F,
                                           -100,
                                           &cfg,
                                           work,
                                           work_bytes,
                                           lines,
                                           8U,
                                           bubbles,
                                           8U,
                                           &summary));
  TEST_ASSERT_EQ(2U, summary.bubble_count);
  TEST_ASSERT_EQ(20U, bubbles[0].y);
  TEST_ASSERT_EQ(140U, bubbles[1].y);
  TEST_ASSERT_EQ(0U, bubbles[0].text_orientation);
}

int main(void)
{
  TEST_BEGIN("test_detector_manga_furigana_and_rtl");
  test_detector_manga_furigana_and_rtl();
  TEST_END("test_detector_manga_furigana_and_rtl");

  TEST_BEGIN("test_detector_webtoon_i8_and_error_guards");
  test_detector_webtoon_i8_and_error_guards();
  TEST_END("test_detector_webtoon_i8_and_error_guards");
  return 0;
}

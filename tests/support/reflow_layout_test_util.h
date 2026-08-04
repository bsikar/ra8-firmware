/**
 * @file reflow_layout_test_util.h
 * @brief Shared engine fixture for the test_ra8_reflow_layout_*_mcdc.c siblings.
 *
 * @details
 * Header-only test fixture providing the shared reflow engine, the baked PNG
 * fixtures and DI image loader, the image-decode scratch arena, and the
 * init / lay / line-count helpers shared by
 * test_ra8_reflow_layout_flow_mcdc.c and
 * test_ra8_reflow_layout_content_mcdc.c. Every paragraph is laid out with
 * the fixed-metric Ahem face so glyph advances are exactly one em -- the
 * geometry (column fill, wrap points, page overflow) is deterministic.
 * Everything here has internal linkage, so each including test executable
 * owns a private engine.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <string.h>

#include "fixture_ahem.h"
#include "ra8_err.h"
#include "ra8_reflow.h"
#include "ra8_reflow_image.h"
#include "unity_minimal.h"

/** @brief Shared engine (large -- keep off the stack). */
static ra8_reflow_t s_eng;

/** @brief Byte capacity of the shared image-decode scratch arena. */
enum : uint32_t {
  k_img_scratch_bytes = 64U * 1024U, /**< 64 KiB bump-arena backing store. */
};

/** @brief Decode scratch for the image-loader path (bump arena). */
static uint8_t s_img_scratch[k_img_scratch_bytes];

/** @brief Named viewport / font geometry (no magic numbers). */
enum : uint16_t {
  k_vp_w       = 200U, /**< Default test viewport width.                    */
  k_vp_h       = 400U, /**< Default test viewport height.                   */
  k_vp_h_short = 96U,  /**< Short viewport to force early page breaks.      */
  k_vp_w_tiny  = 32U,  /**< Width where col_w == 0 (degenerate column).     */
  k_vp_h_tiny  = 32U,  /**< Height where avail_h == 0 (degenerate page).    */
  k_vp_w_half  = 100U, /**< Half the default viewport width (center guard). */
  k_font_px    = 16U,  /**< Ahem body size (1 em advance == 16 px).         */
};

/** @brief Body / link colours for ra8_reflow_init(). */
enum : uint32_t {
  k_body_color = 0xFFFFFFU, /**< White body text. */
  k_link_color = 0x3060FFU, /**< Blue links.      */
};

/**
 * @brief A tall 4x240 RGB PNG (column-fit becomes a tall box).
 * @details Baked with zlib; intrinsic 4 wide by 240 high, so when fit to the
 * text column the box keeps a tall aspect -- big enough to overflow a short
 * page. Channel content is irrelevant (layout only probes the dimensions).
 */
static const uint8_t s_png_tall[] = {
  137, 80,  78,  71,  13,  10,  26,  10,  0,   0,   0,   13, 73, 72,  68, 82, 0,   0,  0,  4,   0,
  0,   0,   240, 8,   2,   0,   0,   0,   168, 144, 82,  38, 0,  0,   0,  26, 73,  68, 65, 84,  120,
  218, 237, 193, 129, 0,   0,   0,   0,   195, 160, 249, 83, 95, 224, 8,  85, 1,   0,  0,  124, 3,
  12,  48,  0,   1,   196, 109, 199, 134, 0,   0,   0,   0,  73, 69,  78, 68, 174, 66, 96, 130,
};

/**
 * @brief A 2x2 RGB PNG: column-fit stays 2x2 (always fits any page).
 * @details Reused from tests/test_ra8_reflow_image.c -- the smallest decodable
 * raster fixture stb_image accepts.
 */
static const uint8_t s_png_2x2[] = {
  137, 80,  78,  71, 13,  10,  26,  10,  0,   0,   0,   13,  73,  72,  68,  82,  0,   0,  0,   2,
  0,   0,   0,   2,  8,   2,   0,   0,   0,   253, 212, 154, 115, 0,   0,   0,   22,  73, 68,  65,
  84,  120, 156, 99, 248, 207, 192, 192, 240, 159, 129, 145, 129, 225, 255, 255, 255, 12, 0,   30,
  246, 4,   253, 9,  237, 52,  62,  0,   0,   0,   0,   73,  69,  78,  68,  174, 66,  96, 130,
};

/** @brief Eight bytes that are not any image format stb_image accepts. */
static const uint8_t s_junk[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};

/** @brief Which baked fixture the DI image loader should return. */
typedef enum : uint8_t {
  k_loader_tall = 0U, /**< Return the tall 4x240 PNG. */
  k_loader_2x2  = 1U, /**< Return the 2x2 PNG.        */
  k_loader_junk = 2U, /**< Return undecodable bytes.  */
} loader_select_t;

/** @brief Loader selector handed to the engine via ctx. */
static loader_select_t s_loader_select = k_loader_tall;

/**
 * @brief DI image loader: resolves any href to the selected baked fixture.
 *
 * @details Wired through ra8_reflow_set_image_loader(). The selector lives in a
 * file-static so each test can pick the fixture (tall / 2x2 / junk) that drives
 * the decision it targets. The bytes are static and outlive the call.
 *
 * @param[in]  ctx       Unused (selector is the file-static s_loader_select).
 * @param[in]  href      Image src (ignored; one fixture per test).
 * @param[in]  href_len  Length of @p href (ignored).
 * @param[out] out_bytes Receives the fixture pointer.
 * @param[out] out_len   Receives the fixture length.
 * @return k_ra8_ok always (the bytes are always supplied).
 */
static inline ra8_err_t test_image_loader(void*           ctx,
                                          const char*     href,
                                          uint32_t        href_len,
                                          const uint8_t** out_bytes,
                                          size_t*         out_len)
{
  (void)ctx;
  (void)href;
  (void)href_len;
  switch (s_loader_select) {
    case k_loader_2x2:
      *out_bytes = s_png_2x2;
      *out_len   = sizeof s_png_2x2;
      break;
    case k_loader_junk:
      *out_bytes = s_junk;
      *out_len   = sizeof s_junk;
      break;
    case k_loader_tall:
    default:
      *out_bytes = s_png_tall;
      *out_len   = sizeof s_png_tall;
      break;
  }
  return k_ra8_ok;
}

/** @brief Init the shared engine at the given viewport (Ahem, body colours). */
static inline void init_engine(uint16_t w, uint16_t h)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(w,
                                 h,
                                 k_fixture_ahem,
                                 (size_t)k_fixture_ahem_len,
                                 k_font_px,
                                 k_body_color,
                                 k_link_color,
                                 &s_eng));
}

/** @brief Lay out @p doc into the shared engine; assert success; return pages. */
static inline uint32_t lay(const char* doc)
{
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_eng, (const uint8_t*)doc, (uint32_t)strlen(doc), &pages));
  return pages;
}

/** @brief Count distinct baseline rows among the laid-out glyphs (line count). */
static inline uint32_t line_count(void)
{
  if (s_eng.glyph_count == 0U) {
    return 0U;
  }
  uint32_t lines = 1U;
  int32_t  py    = s_eng.glyphs[0].y;
  for (uint32_t i = 0U; i < s_eng.glyph_count; ++i) {
    if (s_eng.glyphs[i].y != py) {
      lines++;
      py = s_eng.glyphs[i].y;
    }
  }
  return lines;
}

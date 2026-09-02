/**
 * @file reflow_layout_test_util.h
 * @brief Shared engine fixture for the test_reflow_layout_*_mcdc.c siblings.
 *
 * @details
 * Header-only test fixture providing the shared reflow engine, the baked PNG
 * fixtures and DI image loader, the image-decode scratch arena, and the
 * init / lay / line-count helpers shared by
 * test_reflow_layout_flow_mcdc.c and
 * test_reflow_layout_content_mcdc.c. Every paragraph is laid out with
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
#include "reflow.h"
#include "reflow_image.h"
#include "unity_minimal.h"

/** @brief Shared engine (large -- keep off the stack). */
static reflow_t s_eng;

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

/** @brief Body / link colours for reflow_init(). */
enum : uint32_t {
  k_body_color = 0xFFFFFFU, /**< White body text. */
  k_link_color = 0x3060FFU, /**< Blue links.      */
};

/** @brief Which baked fixture the DI image loader should return. */
typedef enum : uint8_t {
  k_loader_tall       = 0U, /**< Return the tall 4x240 PNG.                   */
  k_loader_2x2        = 1U, /**< Return the 2x2 PNG.                          */
  k_loader_junk       = 2U, /**< Return undecodable bytes.                    */
  k_loader_bmp_zero_w = 3U, /**< Return a header-only BMP declaring width 0.  */
  k_loader_bmp_zero_h = 4U, /**< Return a header-only BMP declaring height 0. */
} loader_select_t;

/** @brief Loader selector handed to the engine via ctx. */
static loader_select_t s_loader_select = k_loader_tall;

/**
 * @brief Serve the baked BMP header that declares a ZERO WIDTH.
 *
 * @details
 * A 54-byte BMP: a 14-byte file header followed by a 40-byte
 * BITMAPINFOHEADER declaring `biWidth = 0`, `biHeight = 1`, one plane, 24
 * bits per pixel and `BI_RGB`. stb_image's header parser accepts that
 * combination and reports the declared dimensions verbatim -- its info path
 * carries no zero-dimension rejection -- so this is the only fixture shape
 * that makes `ra8_img_probe_size()` SUCCEED while handing back a zero edge.
 * That is what isolates the defensive `iw <= 0` guard in
 * `internal_image_resolve_size` from the probe-failure condition beside it:
 * without it, every vector that reaches the guard has already been rejected
 * by the probe.
 *
 * The bytes live inside this provider rather than at file scope so a
 * translation unit that includes this header without using the loader does
 * not carry an unreferenced fixture.
 *
 * @param[out] out_bytes Receives the fixture pointer.
 * @param[out] out_len   Receives the fixture length (54).
 *
 * @return Nothing.
 *
 * @pre Both out-pointers are non-NULL.
 * @pre The caller does not free or write through the returned pointer.
 * @post `*out_bytes` addresses `*out_len` readable bytes.
 * @post The fixture out-lives the call (static storage).
 *
 * @note Reentrant; the storage is immutable.
 * @see test_image_bmp_zero_height()
 * @since 0.1.0
 */
static inline void test_image_bmp_zero_width(const uint8_t** out_bytes, size_t* out_len)
{
  static const uint8_t s_bmp_zero_w[] = {
    0x42U, 0x4DU,               /* "BM"                   */
    0x36U, 0x00U, 0x00U, 0x00U, /* file size              */
    0x00U, 0x00U, 0x00U, 0x00U, /* reserved x2            */
    0x36U, 0x00U, 0x00U, 0x00U, /* pixel-data offset (54) */
    0x28U, 0x00U, 0x00U, 0x00U, /* DIB header size (40)   */
    0x00U, 0x00U, 0x00U, 0x00U, /* biWidth  = 0           */
    0x01U, 0x00U, 0x00U, 0x00U, /* biHeight = 1           */
    0x01U, 0x00U,               /* planes = 1             */
    0x18U, 0x00U,               /* bpp = 24               */
    0x00U, 0x00U, 0x00U, 0x00U, /* compression = BI_RGB   */
    0x00U, 0x00U, 0x00U, 0x00U, /* image size             */
    0x00U, 0x00U, 0x00U, 0x00U, /* x pixels per metre     */
    0x00U, 0x00U, 0x00U, 0x00U, /* y pixels per metre     */
    0x00U, 0x00U, 0x00U, 0x00U, /* colours used           */
    0x00U, 0x00U, 0x00U, 0x00U, /* important colours      */
  };
  *out_bytes = s_bmp_zero_w;
  *out_len   = sizeof s_bmp_zero_w;
}

/**
 * @brief Serve the baked BMP header that declares a ZERO HEIGHT.
 *
 * @details
 * Byte-for-byte the ::test_image_bmp_zero_width fixture with the two
 * dimension fields swapped: `biWidth = 1`, `biHeight = 0`. Keeping them as
 * two separate baked headers rather than one patched template means the
 * bytes a vector feeds the probe are fixed at compile time, so a failing
 * vector can only be the guard under test and never a fixture the previous
 * vector left mutated.
 *
 * This is the fixture that isolates the `ih <= 0` guard, with the probe
 * succeeding and `iw <= 0` false.
 *
 * @param[out] out_bytes Receives the fixture pointer.
 * @param[out] out_len   Receives the fixture length (54).
 *
 * @return Nothing.
 *
 * @pre Both out-pointers are non-NULL.
 * @pre The caller does not free or write through the returned pointer.
 * @post `*out_bytes` addresses `*out_len` readable bytes.
 * @post The fixture out-lives the call (static storage).
 *
 * @note Reentrant; the storage is immutable.
 * @see test_image_bmp_zero_width()
 * @since 0.1.0
 */
static inline void test_image_bmp_zero_height(const uint8_t** out_bytes, size_t* out_len)
{
  static const uint8_t s_bmp_zero_h[] = {
    0x42U, 0x4DU,               /* "BM"                   */
    0x36U, 0x00U, 0x00U, 0x00U, /* file size              */
    0x00U, 0x00U, 0x00U, 0x00U, /* reserved x2            */
    0x36U, 0x00U, 0x00U, 0x00U, /* pixel-data offset (54) */
    0x28U, 0x00U, 0x00U, 0x00U, /* DIB header size (40)   */
    0x01U, 0x00U, 0x00U, 0x00U, /* biWidth  = 1           */
    0x00U, 0x00U, 0x00U, 0x00U, /* biHeight = 0           */
    0x01U, 0x00U,               /* planes = 1             */
    0x18U, 0x00U,               /* bpp = 24               */
    0x00U, 0x00U, 0x00U, 0x00U, /* compression = BI_RGB   */
    0x00U, 0x00U, 0x00U, 0x00U, /* image size             */
    0x00U, 0x00U, 0x00U, 0x00U, /* x pixels per metre     */
    0x00U, 0x00U, 0x00U, 0x00U, /* y pixels per metre     */
    0x00U, 0x00U, 0x00U, 0x00U, /* colours used           */
    0x00U, 0x00U, 0x00U, 0x00U, /* important colours      */
  };
  *out_bytes = s_bmp_zero_h;
  *out_len   = sizeof s_bmp_zero_h;
}

/**
 * @brief DI image loader: resolves any href to the selected baked fixture.
 *
 * @details Wired through reflow_set_image_loader(). The selector lives in a
 * file-static so each test can pick the fixture (tall / 2x2 / junk) that drives
 * the decision it targets. The baked 4x240 RGB PNG preserves a tall aspect when
 * fit to the text column and overflows a short page. The 2x2 RGB PNG, reused
 * from test_reflow_image.c, is the smallest decodable raster fixture and fits
 * every page. The bytes are static and outlive the call.
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
  static const uint8_t s_png_tall[] = {
    137, 80,  78,  71, 13,  10, 26,  10,  0,   0,   0,   13,  73,  72,  68,  82, 0,
    0,   0,   4,   0,  0,   0,  240, 8,   2,   0,   0,   0,   168, 144, 82,  38, 0,
    0,   0,   26,  73, 68,  65, 84,  120, 218, 237, 193, 129, 0,   0,   0,   0,  195,
    160, 249, 83,  95, 224, 8,  85,  1,   0,   0,   124, 3,   12,  48,  0,   1,  196,
    109, 199, 134, 0,  0,   0,  0,   73,  69,  78,  68,  174, 66,  96,  130,
  };
  static const uint8_t s_png_2x2[] = {
    137, 80,  78,  71, 13,  10,  26,  10,  0,   0,   0,   13,  73,  72,  68,  82,  0,   0,  0,   2,
    0,   0,   0,   2,  8,   2,   0,   0,   0,   253, 212, 154, 115, 0,   0,   0,   22,  73, 68,  65,
    84,  120, 156, 99, 248, 207, 192, 192, 240, 159, 129, 145, 129, 225, 255, 255, 255, 12, 0,   30,
    246, 4,   253, 9,  237, 52,  62,  0,   0,   0,   0,   73,  69,  78,  68,  174, 66,  96, 130,
  };
  static const uint8_t s_junk[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
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
    case k_loader_bmp_zero_w:
      test_image_bmp_zero_width(out_bytes, out_len);
      break;
    case k_loader_bmp_zero_h:
      test_image_bmp_zero_height(out_bytes, out_len);
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
                 reflow_init(w,
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
  TEST_ASSERT_EQ(k_ra8_ok,
                 reflow_layout_chapter(&s_eng, (const uint8_t*)doc, (uint32_t)strlen(doc), &pages));
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

/**
 * @file reflow_v1_test_util.h
 * @brief Shared Literata fixture for the v1 test_reflow* siblings.
 *
 * @details
 * Header-only test fixture providing the Literata font loader (from the
 * CMake-provided repository root), the shared
 * engine / framebuffer storage, the XHTML fragments, and the gfx bind /
 * lit-pixel helpers shared by test_reflow.c and
 * test_reflow_api_mcdc.c. Everything here has internal linkage, so each
 * including test executable owns a private engine and framebuffer.
 *
 * These fixtures assert on v1 glyph-array internals that the litehtml v2
 * adapter deliberately does not populate; both including test files are
 * excluded from the build under REFLOW_USE_LITEHTML (see
 * tests/CMakeLists.txt).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_test_file.h"
#include "ra8_test_file_posix.h"
#include "reflow.h"
#include "unity_minimal.h"

/* --------------------------------------------------------------------- */

/**
 * @enum test_reflow_sizes_t
 * @brief Synthetic-fixture sizing constants.
 */
typedef enum : uint32_t {
  k_test_viewport_w    = 320U, /**< Test viewport width, pixels.       */
  k_test_viewport_h    = 240U, /**< Test viewport height, pixels.      */
  k_test_font_px       = 18U,  /**< Default font size for tests.       */
  k_test_font_px_large = 36U,  /**< Larger font size for re-flow test. */
  /** Test fb pixels. */
  k_test_fb_pixels      = (uint32_t)k_test_viewport_w * (uint32_t)k_test_viewport_h,
  k_test_fb_bytes_argb  = k_test_fb_pixels * 4U, /**< 4 BPP framebuffer.                 */
  k_test_font_buf_bytes = 2U * 1024U * 1024U,    /**< 2 MiB font load capacity.          */
  k_test_color_body     = 0x00FFFFFFU,           /**< Body colour (white).               */
  k_test_color_link     = 0x000000FFU,           /**< Link colour (blue).                */
  k_test_origin_dx      = 120U,                  /**< Render-origin test x offset (px).  */
  k_test_origin_dy      = 80U,                   /**< Render-origin test y offset (px).  */
  k_test_pages_poison   = 99U,                   /**< Out-param preload (must be reset). */
  k_test_stub_fill      = 0xA5U,                 /**< Synthetic-font-blob fill byte.     */
} test_reflow_sizes_t;

/* Static storage so the host stack stays small. */
static uint8_t  s_font_buf[k_test_font_buf_bytes];
static size_t   s_font_len = 0U;
static uint32_t s_fb[k_test_fb_pixels];

/* Engine handle is large; keep it static. */
static reflow_t s_engine;

/* --------------------------------------------------------------------- */
/* XHTML fixtures */
/* --------------------------------------------------------------------- */

static const char* const s_xhtml_simple = "<html><body><h1>Title</h1><p>Body</p></body></html>";

static const char* const s_xhtml_multi = "<html><body>"
                                         "<p>Paragraph one with several words to wrap.</p>"
                                         "<p>Paragraph two follows on the next block.</p>"
                                         "<p>Paragraph three closes out the body content.</p>"
                                         "</body></html>";

static inline const char* test_xhtml_styled(void)
{
  return "<html><body><p>plain <b>bold</b> and <i>italic</i> mix</p></body></html>";
}

/* --------------------------------------------------------------------- */
/* Helpers */
/* --------------------------------------------------------------------- */

/**
 * @brief Load the Literata font into `s_font_buf`.
 *
 * @return true if loaded, false if the font could not be found.

 * @details Performs one bounded, deterministic operation for this host test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline bool internal_priv_load_font(void)
{
  static uint8_t               s_font_staging[k_test_font_buf_bytes];
  const ra8_test_file_result_t result =
    internal_test_file_read(RA8_TEST_REPO_ROOT "/libs/ra8_fonts/Literata-Regular.ttf",
                            s_font_buf,
                            sizeof(s_font_buf),
                            s_font_staging,
                            sizeof(s_font_staging));
  if ((result.status != k_ra8_test_file_ok) || (result.transferred < 16U)) {
    return false;
  }
  s_font_len = result.transferred;
  return true;
}

/**
 * @brief Bind ra8_gfx to the local test framebuffer.

 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline void internal_priv_bind_gfx(void)
{
  (void)memset(s_fb, 0, sizeof(s_fb));
  ra8_err_t err = ra8_gfx_init(s_fb,
                               (uint16_t)k_test_viewport_w,
                               (uint16_t)k_test_viewport_h,
                               k_ra8_gfx_format_argb8888);
  TEST_ASSERT_EQ(k_ra8_ok, err);
}

/**
 * @brief Count non-zero framebuffer pixels in a rectangular region.

 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] x0 Argument for the bounded test operation.
 * @param[in] y0 Argument for the bounded test operation.
 * @param[in] x1 Argument for the bounded test operation.
 * @param[in] y1 Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline uint32_t
internal_priv_count_lit_pixels(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
  uint32_t lit = 0U;
  for (int32_t y = y0; y < y1; ++y) {
    for (int32_t x = x0; x < x1; ++x) {
      if (s_fb[((uint32_t)y * (uint32_t)k_test_viewport_w) + (uint32_t)x] != 0U) {
        ++lit;
      }
    }
  }
  return lit;
}

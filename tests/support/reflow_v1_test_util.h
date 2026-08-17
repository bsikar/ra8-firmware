/**
 * @file reflow_v1_test_util.h
 * @brief Shared Literata fixture for the v1 test_ra8_reflow* siblings.
 *
 * @details
 * Header-only test fixture providing the Literata font loader (firmware root
 * derived from __FILE__ so it works from any cmake build dir), the shared
 * engine / framebuffer storage, the XHTML fragments, and the gfx bind /
 * lit-pixel helpers shared by test_ra8_reflow.c and
 * test_ra8_reflow_api_mcdc.c. Everything here has internal linkage, so each
 * including test executable owns a private engine and framebuffer.
 *
 * These fixtures assert on v1 glyph-array internals that the litehtml v2
 * adapter deliberately does not populate; both including test files are
 * excluded from the build under RA8_REFLOW_USE_LITEHTML (see
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
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_reflow.h"
#include "support/ra8_test_file.h"
#include "support/ra8_test_file_posix.h"
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
  k_test_root_path_max  = 1024U,                 /**< Max derived firmware-root path.    */
  k_test_origin_dx      = 120U,                  /**< Render-origin test x offset (px).  */
  k_test_origin_dy      = 80U,                   /**< Render-origin test y offset (px).  */
  k_test_font_rel_max   = 64U,                   /**< Room for the font's relative path. */
  k_test_pages_poison   = 99U,                   /**< Out-param preload (must be reset). */
  k_test_stub_fill      = 0xA5U,                 /**< Synthetic-font-blob fill byte.     */
} test_reflow_sizes_t;

/* Static storage so the host stack stays small. */
static uint8_t  s_font_buf[k_test_font_buf_bytes];
static uint8_t  s_font_staging[k_test_font_buf_bytes];
static size_t   s_font_len = 0U;
static uint32_t s_fb[k_test_fb_pixels];

/* Engine handle is large; keep it static. */
static ra8_reflow_t s_engine;

/* --------------------------------------------------------------------- */
/* XHTML fixtures */
/* --------------------------------------------------------------------- */

static const char* const s_xhtml_simple = "<html><body><h1>Title</h1><p>Body</p></body></html>";

static const char* const s_xhtml_multi = "<html><body>"
                                         "<p>Paragraph one with several words to wrap.</p>"
                                         "<p>Paragraph two follows on the next block.</p>"
                                         "<p>Paragraph three closes out the body content.</p>"
                                         "</body></html>";

static const char* const s_xhtml_styled =
  "<html><body><p>plain <b>bold</b> and <i>italic</i> mix</p></body></html>";

/* --------------------------------------------------------------------- */
/* Helpers */
/* --------------------------------------------------------------------- */

/**
 * @brief Strip the trailing path component from `path`, in place.

 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] path Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline void internal_priv_dirname_inplace(char* path)
{
  size_t len = strlen(path);
  while (len > 0U && path[len - 1U] != '/') {
    --len;
  }
  if (len > 0U) {
    path[len - 1U] = '\0';
  }
}

/**
 * @brief Compute the firmware root from __FILE__ (which is the
 *        absolute path of this header under the cmake build).

 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] out Argument for the bounded test operation.
 * @param[in] out_cap Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline void internal_priv_resolve_fw_root(char* out, size_t out_cap)
{
  /* __FILE__ expands where the macro is spelled, so it resolves to
   * ".../tests/support/reflow_v1_test_util.h" -- strip three trailing
   * components to land on the firmware root. */
  (void)snprintf(out, out_cap, "%s", __FILE__);
  internal_priv_dirname_inplace(out); /* drop reflow_v1_test_util.h */
  internal_priv_dirname_inplace(out); /* drop support/              */
  internal_priv_dirname_inplace(out); /* drop tests/                */
}

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
  static char s_root[k_test_root_path_max];
  internal_priv_resolve_fw_root(s_root, sizeof(s_root));

  static char s_path[k_test_root_path_max + k_test_font_rel_max];
  /* GCC's -Wformat-truncation flags the maximally-conservative case
   * where `s_root` is the full 1 KiB; concatenate manually so the
   * checker can see the bound. */
  const char* const k_font_rel = "/libs/ra8_fonts/Literata-Regular.ttf";
  size_t            root_len   = 0U;
  while (root_len + 1U < sizeof(s_path) && s_root[root_len] != '\0') {
    s_path[root_len] = s_root[root_len];
    ++root_len;
  }
  size_t rel_len = 0U;
  while (root_len + rel_len + 1U < sizeof(s_path) && k_font_rel[rel_len] != '\0') {
    s_path[root_len + rel_len] = k_font_rel[rel_len];
    ++rel_len;
  }
  s_path[root_len + rel_len]          = '\0';
  const ra8_test_file_result_t result = internal_test_file_read(s_path,
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

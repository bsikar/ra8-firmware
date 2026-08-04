/**
 * @file bench_ra8_gfx_text.c
 * @brief Microbenchmark: text rendering through the bundled bitmap-font
 *        path of `ra8_gfx_text_out`.
 *
 * @details
 * The repo ships a bundled IBM-PC VGA bitmap font (`ra8_gfx_font_8x16`)
 * and the stb_truetype headers for future TTF support. The current
 * production code path uses the bundled bitmap font, so that is what
 * this benchmark exercises end-to-end: bind a 256x64 RGB565
 * framebuffer, then render the classic pangram "The quick brown fox
 * jumps over the lazy dog." in a tight loop.
 *
 * Bytes-per-iteration is the rendered pixel area (width * height * bpp)
 * so MB/s is directly comparable to a memcpy throughput baseline.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_bench.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"

/**
 * @enum bench_text_dims_t
 * @brief Off-screen framebuffer dimensions used by the benchmark.
 */
typedef enum : uint16_t {
  k_bench_text_fb_w = 512U, /**< Bench text fb w. */
  k_bench_text_fb_h = 32U,  /**< Bench text fb h. */
  k_bench_text_bpp  = 2U,   /**< RGB565.          */
} bench_text_dims_t;

/**
 * @enum bench_text_colors_t
 * @brief Colours used for fg/bg in the rendered string.
 */
typedef enum : uint32_t {
  k_bench_text_fg = 0x00FFFFFFU, /**< Bench text fg. */
  k_bench_text_bg = 0x00000000U, /**< Bench text bg. */
} bench_text_colors_t;

static uint8_t
  s_fb[(uint32_t)k_bench_text_fb_w * (uint32_t)k_bench_text_fb_h * (uint32_t)k_bench_text_bpp];

/**
 * @brief Bench entry.
 */
/* The nesting this reports is entirely inside RA8_BENCH_TIME's expansion --
 * the harness wraps every measured body in do{ while{ for{ ... } } } to pick an
 * iteration count that reaches a minimum wall time. The body below is flat
 * straight-line code and contributes no nesting of its own.
 *
 * It is not reducible: an adaptive iteration count needs a calibration loop
 * around the measured loop, and the body must stay lexically inline -- routing
 * it through a callback would add an indirect call per iteration and inflate
 * exactly the number the benchmark exists to report. clang-tidy offers no
 * per-aspect suppression, so this also masks the statement count for this
 * function; keep it short.
 */
// NOLINTNEXTLINE(readability-function-size)
int main(void)
{
  ra8_bench_print_header("bench_ra8_gfx_text");
  ra8_err_t e = ra8_gfx_init(s_fb,
                             (uint16_t)k_bench_text_fb_w,
                             (uint16_t)k_bench_text_fb_h,
                             k_ra8_gfx_format_rgb565);
  if (e != k_ra8_ok) {
    (void)fprintf(stderr, "ra8_gfx_init failed (%d)\n", (int)e);
    return 1;
  }

  static const char k_str[] = "The quick brown fox jumps over the lazy dog.";

  uint64_t bytes_per_iter =
    (uint64_t)k_bench_text_fb_w * (uint64_t)k_bench_text_fb_h * (uint64_t)k_bench_text_bpp;

  RA8_BENCH_TIME("gfx_text_pangram_8x16_rgb565", bytes_per_iter, {
    (void)ra8_gfx_clear((uint32_t)k_bench_text_bg);
    (void)ra8_gfx_text_out(0,
                           0,
                           k_str,
                           &ra8_gfx_font_8x16,
                           (uint32_t)k_bench_text_fg,
                           (uint32_t)k_bench_text_bg);
  });
  return 0;
}

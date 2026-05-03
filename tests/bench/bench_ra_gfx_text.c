/**
 * @file bench_ra_gfx_text.c
 * @brief Microbenchmark: text rendering through the bundled bitmap-font
 *        path of `ra_gfx_text_out`.
 *
 * @details
 * The repo ships a bundled IBM-PC VGA bitmap font (`ra_gfx_font_8x16`)
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
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "ra_bench.h"
#include "ra_err.h"
#include "ra_gfx.h"
#include "ra_gfx_font.h"

/**
 * @enum bench_text_dims_t
 * @brief Off-screen framebuffer dimensions used by the benchmark.
 */
typedef enum : uint16_t {
  k_bench_text_fb_w = 512U,
  k_bench_text_fb_h = 32U,
  k_bench_text_bpp  = 2U, /* RGB565 */
} bench_text_dims_t;

/**
 * @enum bench_text_colors_t
 * @brief Colours used for fg/bg in the rendered string.
 */
typedef enum : uint32_t {
  k_bench_text_fg = 0x00FFFFFFU,
  k_bench_text_bg = 0x00000000U,
} bench_text_colors_t;

static uint8_t
  s_fb[(uint32_t)k_bench_text_fb_w * (uint32_t)k_bench_text_fb_h * (uint32_t)k_bench_text_bpp];

/**
 * @brief Bench entry.
 */
int main(void)
{
  ra_bench_print_header("bench_ra_gfx_text");
  ra_err_t e = ra_gfx_init(s_fb,
                           (uint16_t)k_bench_text_fb_w,
                           (uint16_t)k_bench_text_fb_h,
                           k_ra_gfx_format_rgb565);
  if (e != k_ra_ok) {
    (void)fprintf(stderr, "ra_gfx_init failed (%d)\n", (int)e);
    return 1;
  }

  static const char k_str[] = "The quick brown fox jumps over the lazy dog.";

  uint64_t bytes_per_iter =
    (uint64_t)k_bench_text_fb_w * (uint64_t)k_bench_text_fb_h * (uint64_t)k_bench_text_bpp;

  RA_BENCH_TIME("gfx_text_pangram_8x16_rgb565", bytes_per_iter, {
    (void)ra_gfx_clear((uint32_t)k_bench_text_bg);
    (void)ra_gfx_text_out(0,
                          0,
                          k_str,
                          &ra_gfx_font_8x16,
                          (uint32_t)k_bench_text_fg,
                          (uint32_t)k_bench_text_bg);
  });
  return 0;
}
